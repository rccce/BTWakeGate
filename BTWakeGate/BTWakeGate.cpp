/*
 * BTWakeGate.kext r1
 *
 * Lets bluetoothd find the Intel USB Bluetooth controller (here an AX210, 8087:0032, with
 * IntelBluetoothFirmware) right after a wake, and resets the controller when it survived a sleep
 * without answering.
 *
 * Without it (logs of 2026-11-19 and 2026-11-20, Hackintosh report section 8.12):
 *  - Mode A, after most sleeps: the controller lost power during the sleep. After the wake its USB
 *    device is destroyed (+0.2 to +0.3 s), the running bluetoothd gets read errors and exits, and
 *    launchd starts a new bluetoothd at once. The new one calls IOKitWaitQuiet() (the registry is
 *    quiet at that moment), looks for a Bluetooth dongle once (+0.6 s) and finds none: the
 *    controller is enumerated again only at +1.5 s, and IntelBluetoothFirmware needs about 1.4 s
 *    more for the firmware. Then it polls for Apple's internal controller only, gives up after
 *    15 s and exits; the next bluetoothd finds the controller. Bluetooth is back about 16.5 s
 *    after the wake.
 *  - Mode B, seen once, after a sleep of 2.7 s: the USB device of the controller was not
 *    destroyed, but the controller answered nothing (0xe00002ed on every endpoint, endpoint 0
 *    included). Nothing reset it, every new bluetoothd found a dead controller, and Bluetooth
 *    stayed dead until a restart.
 *
 * What r1 does:
 *  1. Gate. At kIOMessageSystemWillPowerOn (the first wake message, sent before the device tree is
 *     powered) it makes itself busy with IOService::adjustBusy(+1), if a controller was there before
 *     the sleep. IOKitWaitQuiet() waits until no IOService is busy, so a bluetoothd started during
 *     the wake waits there instead of looking too early. The gate opens as soon as a new controller
 *     device (same vendor, a listed product, the same USB location, another registry entry than
 *     before the sleep) has finished matching and is not busy: IntelBluetoothFirmware::start()
 *     uploads the firmware before it returns. It also opens after GateMaxMs, when the Mac goes to
 *     sleep again, and when the driver stops.
 *  2. Reset. If SurvivorDelayMs after the wake the controller from before the sleep is still there
 *     although the Mac really slept (calendar time minus uptime since the sleep message is at least
 *     MinSleepMs), it calls IOUSBHostDevice::reset() on it from a separate thread: the port
 *     re-enumerates the controller and IntelBluetoothFirmware uploads the firmware again. The gate
 *     stays closed until the new controller is ready, at most GateMaxMs after the reset. At most
 *     one reset per wake and MaxResets resets in 10 minutes. After a sleep that did not really
 *     happen (aborted) the controller is left alone.
 *  With Enabled false it only records (no gate, no reset).
 *
 * Results: ioreg -rw0 -c BTWakeGate -l -d 1 (State, LastWake, History, Wakes, Holds, Resets).
 * Kernel log lines start with "BTWakeGate:" (they may not reach the unified log for kexts injected
 * by OpenCore). Times in LastWake and History are UTC.
 *
 * r1 has only had a syntax check and a simulation of its state machine on another system; it has
 * not been compiled with the macOS SDK or run on macOS yet.
 */

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/usb/IOUSBHostDevice.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/libkern.h>
#include <kern/clock.h>
#include <kern/thread_call.h>

#define BWG_VERSION "r1"

namespace {

constexpr UInt64 kNever = ~0ULL;
constexpr UInt32 kPollFastMS = 100;          // while the gate is closed, and in the first 10 s
constexpr UInt32 kPollSlowMS = 500;          // afterwards, to see when the controller is back
constexpr UInt64 kFastPhaseMS = 10000;
constexpr UInt64 kMinRecordMS = 3000;        // a wake is written down at least 3 s after it
constexpr UInt64 kFollowMaxMS = 30000;       // and at the latest 30 s after it
constexpr UInt64 kResetWindowMS = 600000;    // MaxResets counts the resets of the last 10 minutes
constexpr unsigned kMaxProducts = 8;
constexpr unsigned kHistoryMax = 12;
constexpr unsigned kResetSlots = 8;

UInt64 uptimeMS()
{
    uint64_t abstime = 0;
    uint64_t ns = 0;
    clock_get_uptime(&abstime);
    absolutetime_to_nanoseconds(abstime, &ns);
    return ns / 1000000ULL;
}

// Wall clock; unlike the uptime it also counts the time asleep.
UInt64 calendarMS()
{
    clock_sec_t secs = 0;
    clock_usec_t usecs = 0;
    clock_get_calendar_microtime(&secs, &usecs);
    return static_cast<UInt64>(secs) * 1000ULL + static_cast<UInt64>(usecs) / 1000ULL;
}

// "YYYY-MM-DD HH:MM:SS UTC" from Unix seconds (H. Hinnant's civil_from_days, integers only).
void formatUTC(UInt64 unixSeconds, char *out, size_t size)
{
    SInt64 z = static_cast<SInt64>(unixSeconds / 86400ULL) + 719468;
    UInt32 rem = static_cast<UInt32>(unixSeconds % 86400ULL);
    SInt64 era = (z >= 0 ? z : z - 146096) / 146097;
    UInt32 doe = static_cast<UInt32>(z - era * 146097);
    UInt32 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    SInt64 y = static_cast<SInt64>(yoe) + era * 400;
    UInt32 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    UInt32 mp = (5 * doy + 2) / 153;
    UInt32 d = doy - (153 * mp + 2) / 5 + 1;
    UInt32 m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2)
        y++;
    snprintf(out, size, "%04lld-%02u-%02u %02u:%02u:%02u UTC", static_cast<long long>(y), m, d,
             rem / 3600, (rem / 60) % 60, rem % 60);
}

// "+1.54 s" (offset after the wake)
void formatOffset(UInt64 ms, char *out, size_t size)
{
    snprintf(out, size, "+%llu.%02llu s", ms / 1000ULL, (ms % 1000ULL) / 10ULL);
}

// "2.7 s", "12 min", "12.8 h" (no floating point in the kernel)
void formatDuration(UInt64 ms, char *out, size_t size)
{
    if (ms < 60000ULL)
        snprintf(out, size, "%llu.%llu s", ms / 1000ULL, (ms % 1000ULL) / 100ULL);
    else if (ms < 3600000ULL)
        snprintf(out, size, "%llu min", (ms + 30000ULL) / 60000ULL);
    else
        snprintf(out, size, "%llu.%llu h", ms / 3600000ULL, (ms % 3600000ULL) / 360000ULL);
}

UInt32 numberProperty(IORegistryEntry *entry, const char *key, UInt32 fallback)
{
    OSNumber *number = OSDynamicCast(OSNumber, entry->getProperty(key));
    return number != nullptr ? number->unsigned32BitValue() : fallback;
}

UInt32 clampValue(UInt32 value, UInt32 low, UInt32 high)
{
    return value < low ? low : (value > high ? high : value);
}

}  // namespace

class BTWakeGate : public IOService {
    OSDeclareDefaultStructors(BTWakeGate);

public:
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;

private:
    // Options (personality in Info.plist)
    bool enabled {true};
    bool resetSurvivor {true};
    UInt32 vendorID {0x8087};
    UInt32 productIDs[kMaxProducts] {};
    unsigned productCount {0};
    UInt32 gateMaxMS {8000};
    UInt32 survivorDelayMS {2500};
    UInt32 minSleepMS {1000};
    UInt32 maxResets {3};

    IOWorkLoop *workLoop {nullptr};
    IOTimerEventSource *timer {nullptr};
    IONotifier *powerNotifier {nullptr};
    thread_call_t resetCall {nullptr};
    IOWorkLoop *resetLoop {nullptr};     // retained while a reset runs
    bool resetInFlight {false};
    bool stopping {false};
    OSArray *history {nullptr};

    // Noted at kIOMessageSystemWillSleep. All state below changes only on the work loop.
    bool sleepSeen {false};
    IOService *oldDevice {nullptr};      // retained: the controller before the sleep
    UInt64 oldID {0};
    UInt32 oldLocation {0};
    bool oldHasLocation {false};
    UInt64 sleepUptime {0};
    UInt64 sleepCalendar {0};

    // The wake being followed. Times are milliseconds after kIOMessageSystemWillPowerOn.
    bool following {false};
    bool holding {false};
    bool closePending {false};           // close the gate on the first timer tick (not on the PM thread)
    bool keptOld {false};                // the gate opened because the old controller stayed
    UInt64 wakeUptime {0};
    UInt64 holdUptime {0};
    UInt64 deadline {0};
    UInt64 goneAt {kNever};
    UInt64 readyAt {kNever};
    UInt64 resetAt {kNever};
    UInt64 holdMS {0};
    UInt64 sleptAtDecision {kNever};
    IOReturn resetResult {kIOReturnSuccess};
    bool resetReturned {false};
    int firmware {-1};                   // FirmwareLoaded of the new controller: -1 unknown, 0 no, 1 yes
    const char *holdEnd {nullptr};       // why the gate opened (string literals only)

    UInt32 wakeCount {0};
    UInt32 holdCount {0};
    UInt32 resetCount {0};
    UInt64 resetTimes[kResetSlots] {};
    unsigned resetSlot {0};

    void readOptions();
    bool productMatches(UInt32 product) const;
    bool isController(IOService *service) const;
    IOService *copyController(bool newOnly, bool *quiet, int *loaded);
    void forgetOldDevice();
    void closeGate();
    void openGate(const char *why);
    void onTick();
    void finishWake(const char *why);
    bool resetAllowed(UInt64 now) const;
    void sendReset(UInt64 t, UInt64 now);
    UInt64 sleptMS(UInt64 now) const;
    void publishState(const char *state);
    void publishCounters();
    void addHistory(const char *line);
    void releaseResources();

    static void timerAction(OSObject *owner, IOTimerEventSource *sender);
    static IOReturn willSleepAction(OSObject *owner, void *arg0, void *arg1, void *arg2, void *arg3);
    static IOReturn willPowerOnAction(OSObject *owner, void *arg0, void *arg1, void *arg2, void *arg3);
    static IOReturn resetDoneAction(OSObject *owner, void *arg0, void *arg1, void *arg2, void *arg3);
    static IOReturn stopAction(OSObject *owner, void *arg0, void *arg1, void *arg2, void *arg3);
    static IOReturn powerMessage(void *target, void *refCon, UInt32 messageType,
                                 IOService *provider, void *messageArgument, vm_size_t argSize);
    static void resetThread(thread_call_param_t param0, thread_call_param_t param1);
};

OSDefineMetaClassAndStructors(BTWakeGate, IOService);

bool BTWakeGate::start(IOService *provider)
{
    if (!IOService::start(provider))
        return false;

    readOptions();
    history = OSArray::withCapacity(kHistoryMax + 1);
    workLoop = IOWorkLoop::workLoop();
    if (workLoop != nullptr)
        timer = IOTimerEventSource::timerEventSource(this, &BTWakeGate::timerAction);
    if (history == nullptr || timer == nullptr || workLoop->addEventSource(timer) != kIOReturnSuccess) {
        IOLog("BTWakeGate: could not create the work loop\n");
        releaseResources();
        return false;
    }
    resetCall = thread_call_allocate(&BTWakeGate::resetThread, this);

    // Root domain priority interest: kIOMessageSystemWillSleep last on the way down,
    // kIOMessageSystemWillPowerOn first on the way up (before the USB devices are powered).
    powerNotifier = registerPrioritySleepWakeInterest(&BTWakeGate::powerMessage, this, nullptr);
    if (powerNotifier == nullptr) {
        IOLog("BTWakeGate: could not register for sleep and wake messages\n");
        releaseResources();
        return false;
    }

    char config[200];
    char products[64];
    size_t used = 0;
    products[0] = '\0';
    for (unsigned i = 0; i < productCount && used < sizeof(products); i++) {
        int n = snprintf(products + used, sizeof(products) - used, i == 0 ? "%04x" : ",%04x", productIDs[i]);
        if (n < 0)
            break;
        used += static_cast<size_t>(n);
    }
    snprintf(config, sizeof(config),
             "Enabled %s, ResetSurvivor %s, controller %04x:%s, GateMaxMs %u, SurvivorDelayMs %u, MinSleepMs %u, MaxResets %u",
             enabled ? "yes" : "no", resetSurvivor ? "yes" : "no", vendorID, products,
             gateMaxMS, survivorDelayMS, minSleepMS, maxResets);
    setProperty("Version", BWG_VERSION);
    setProperty("Config", config);
    if (resetCall == nullptr)
        setProperty("Warning", "no thread call: the reset step is off");
    publishCounters();
    publishState("idle");
    IOLog("BTWakeGate: %s started (%s)\n", BWG_VERSION, config);
    return true;
}

void BTWakeGate::stop(IOService *provider)
{
    if (workLoop != nullptr)
        workLoop->runAction(&BTWakeGate::stopAction, this);
    releaseResources();
    IOService::stop(provider);
}

void BTWakeGate::free()
{
    releaseResources();
    if (history != nullptr) {
        history->release();
        history = nullptr;
    }
    IOService::free();
}

void BTWakeGate::releaseResources()
{
    if (powerNotifier != nullptr) {
        powerNotifier->remove();
        powerNotifier = nullptr;
    }
    if (timer != nullptr) {
        timer->cancelTimeout();
        if (workLoop != nullptr)
            workLoop->removeEventSource(timer);
        timer->release();
        timer = nullptr;
    }
    if (workLoop != nullptr) {
        workLoop->release();         // a running reset keeps its own reference (resetLoop)
        workLoop = nullptr;
    }
    // A running reset holds a reference to this object, so free() cannot come before it ends;
    // stop() may, and then the thread call is left allocated.
    if (resetCall != nullptr && !resetInFlight) {
        thread_call_cancel(resetCall);
        thread_call_free(resetCall);
        resetCall = nullptr;
    }
    if (holding) {
        holding = false;
        adjustBusy(-1);
    }
    forgetOldDevice();
}

void BTWakeGate::readOptions()
{
    OSBoolean *flag = OSDynamicCast(OSBoolean, getProperty("Enabled"));
    if (flag != nullptr)
        enabled = flag->isTrue();
    flag = OSDynamicCast(OSBoolean, getProperty("ResetSurvivor"));
    if (flag != nullptr)
        resetSurvivor = flag->isTrue();
    vendorID = numberProperty(this, "VendorID", 0x8087) & 0xFFFF;
    productCount = 0;
    OSArray *products = OSDynamicCast(OSArray, getProperty("ProductIDs"));
    if (products != nullptr) {
        for (unsigned i = 0; i < products->getCount() && productCount < kMaxProducts; i++) {
            OSNumber *number = OSDynamicCast(OSNumber, products->getObject(i));
            if (number != nullptr)
                productIDs[productCount++] = number->unsigned32BitValue() & 0xFFFF;
        }
    }
    if (productCount == 0) {
        productIDs[0] = 0x0032;
        productCount = 1;
    }
    gateMaxMS = clampValue(numberProperty(this, "GateMaxMs", 8000), 1000, 20000);
    survivorDelayMS = clampValue(numberProperty(this, "SurvivorDelayMs", 2500), 1000, 10000);
    minSleepMS = clampValue(numberProperty(this, "MinSleepMs", 1000), 200, 60000);
    maxResets = clampValue(numberProperty(this, "MaxResets", 3), 0, kResetSlots);
}

bool BTWakeGate::productMatches(UInt32 product) const
{
    for (unsigned i = 0; i < productCount; i++)
        if (productIDs[i] == product)
            return true;
    return false;
}

bool BTWakeGate::isController(IOService *service) const
{
    OSNumber *vendor = OSDynamicCast(OSNumber, service->getProperty("idVendor"));
    OSNumber *product = OSDynamicCast(OSNumber, service->getProperty("idProduct"));
    return vendor != nullptr && product != nullptr && vendor->unsigned32BitValue() == vendorID &&
           productMatches(product->unsigned32BitValue());
}

// Returns, retained, an active controller device that has finished matching (getMatchingServices
// only lists matched services; IntelBluetoothFirmware::start() returns after the firmware upload).
// newOnly: not the device from before the sleep, and at its USB location if it had one.
IOService *BTWakeGate::copyController(bool newOnly, bool *quiet, int *loaded)
{
    OSDictionary *matching = IOService::serviceMatching("IOUSBHostDevice");
    if (matching == nullptr)
        return nullptr;
    OSIterator *iter = IOService::getMatchingServices(matching);
    matching->release();
    if (iter == nullptr)
        return nullptr;

    IOService *found = nullptr;
    OSObject *object = nullptr;
    while (found == nullptr && (object = iter->getNextObject()) != nullptr) {
        IOService *service = OSDynamicCast(IOService, object);
        if (service == nullptr || service->isInactive() || !isController(service))
            continue;
        if (newOnly) {
            if (service->getRegistryEntryID() == oldID)
                continue;
            if (oldHasLocation) {
                OSNumber *location = OSDynamicCast(OSNumber, service->getProperty("locationID"));
                if (location != nullptr && location->unsigned32BitValue() != oldLocation)
                    continue;
            }
        }
        found = service;
        found->retain();
    }
    iter->release();

    if (found != nullptr) {
        if (quiet != nullptr)
            *quiet = found->getBusyState() == 0;
        if (loaded != nullptr) {
            OSBoolean *flag = OSDynamicCast(OSBoolean, found->getProperty("FirmwareLoaded"));
            *loaded = (flag != nullptr && flag->isTrue()) ? 1 : 0;
        }
    }
    return found;
}

void BTWakeGate::forgetOldDevice()
{
    if (oldDevice != nullptr) {
        oldDevice->release();
        oldDevice = nullptr;
    }
    oldID = 0;
    oldLocation = 0;
    oldHasLocation = false;
}

void BTWakeGate::closeGate()
{
    if (holding)
        return;
    holding = true;
    holdUptime = uptimeMS();
    holdCount++;
    adjustBusy(1);
}

void BTWakeGate::openGate(const char *why)
{
    if (!holding)
        return;
    holding = false;
    holdMS = uptimeMS() - holdUptime;
    holdEnd = why;
    adjustBusy(-1);
    publishState("following the wake, gate open");
}

UInt64 BTWakeGate::sleptMS(UInt64 now) const
{
    UInt64 calendar = calendarMS();
    UInt64 wall = calendar > sleepCalendar ? calendar - sleepCalendar : 0;
    UInt64 up = now > sleepUptime ? now - sleepUptime : 0;
    return wall > up ? wall - up : 0;
}

bool BTWakeGate::resetAllowed(UInt64 now) const
{
    unsigned recent = 0;
    for (unsigned i = 0; i < kResetSlots; i++)
        if (resetTimes[i] != 0 && now - resetTimes[i] < kResetWindowMS)
            recent++;
    return recent < maxResets;
}

void BTWakeGate::sendReset(UInt64 t, UInt64 now)
{
    if (resetCall == nullptr || resetInFlight || oldDevice == nullptr || workLoop == nullptr) {
        keptOld = true;
        openGate("old controller kept, reset not possible");
        return;
    }
    resetAt = t;
    deadline = t + gateMaxMS;
    resetInFlight = true;
    resetReturned = false;
    resetCount++;
    resetTimes[resetSlot] = now;
    resetSlot = (resetSlot + 1) % kResetSlots;
    resetLoop = workLoop;
    resetLoop->retain();
    retain();
    oldDevice->retain();
    if (thread_call_enter1(resetCall, oldDevice)) {
        // Already queued; cannot happen while resetInFlight was false. Drop the extra references.
        oldDevice->release();
        release();
        resetLoop->release();
        resetLoop = nullptr;
    }
    IOLog("BTWakeGate: the controller stayed after a real sleep, resetting its USB port\n");
    publishCounters();
    publishState("holding: reset sent, waiting for the new controller");
}

// Separate thread: IOUSBHostDevice::reset() terminates the device and its children.
void BTWakeGate::resetThread(thread_call_param_t param0, thread_call_param_t param1)
{
    BTWakeGate *self = static_cast<BTWakeGate *>(param0);
    IOService *service = static_cast<IOService *>(param1);
    IOReturn result = kIOReturnUnsupported;
    IOUSBHostDevice *device = OSDynamicCast(IOUSBHostDevice, service);
    if (device != nullptr)
        result = device->isInactive() ? kIOReturnNoDevice : device->reset();

    IOWorkLoop *loop = self->resetLoop;
    if (loop != nullptr) {
        loop->runAction(&BTWakeGate::resetDoneAction, self,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(static_cast<UInt32>(result))));
        loop->release();
    }
    if (service != nullptr)
        service->release();
    self->release();
}

IOReturn BTWakeGate::resetDoneAction(OSObject *owner, void *arg0, void *arg1, void *arg2, void *arg3)
{
    (void)arg1; (void)arg2; (void)arg3;
    BTWakeGate *self = OSDynamicCast(BTWakeGate, owner);
    if (self == nullptr)
        return kIOReturnSuccess;
    self->resetLoop = nullptr;           // the thread releases its reference right after this
    self->resetInFlight = false;
    self->resetReturned = true;
    self->resetResult = static_cast<IOReturn>(static_cast<UInt32>(reinterpret_cast<uintptr_t>(arg0)));
    IOLog("BTWakeGate: reset returned 0x%08x\n", static_cast<UInt32>(self->resetResult));
    if (self->resetResult == kIOReturnNoDevice) {
        // The controller went away by itself in the meantime: keep waiting for the new one.
    } else if (self->resetResult != kIOReturnSuccess) {
        self->keptOld = true;
        self->openGate("reset refused");
    }
    return kIOReturnSuccess;
}

IOReturn BTWakeGate::powerMessage(void *target, void *refCon, UInt32 messageType,
                                  IOService *provider, void *messageArgument, vm_size_t argSize)
{
    (void)refCon; (void)provider; (void)messageArgument; (void)argSize;
    BTWakeGate *self = static_cast<BTWakeGate *>(target);
    if (self == nullptr || self->workLoop == nullptr)
        return kIOReturnSuccess;
    if (messageType == kIOMessageSystemWillSleep)
        self->workLoop->runAction(&BTWakeGate::willSleepAction, self);
    else if (messageType == kIOMessageSystemWillPowerOn)
        self->workLoop->runAction(&BTWakeGate::willPowerOnAction, self);
    return kIOReturnSuccess;        // acknowledged at once
}

IOReturn BTWakeGate::willSleepAction(OSObject *owner, void *arg0, void *arg1, void *arg2, void *arg3)
{
    (void)arg0; (void)arg1; (void)arg2; (void)arg3;
    BTWakeGate *self = OSDynamicCast(BTWakeGate, owner);
    if (self == nullptr || self->stopping)
        return kIOReturnSuccess;
    if (self->following)
        self->finishWake("the Mac went to sleep again");
    self->forgetOldDevice();
    IOService *device = self->copyController(false, nullptr, nullptr);
    if (device != nullptr) {
        self->oldDevice = device;            // keeps the reference from copyController
        self->oldID = device->getRegistryEntryID();
        OSNumber *location = OSDynamicCast(OSNumber, device->getProperty("locationID"));
        if (location != nullptr) {
            self->oldLocation = location->unsigned32BitValue();
            self->oldHasLocation = true;
        }
    }
    self->sleepUptime = uptimeMS();
    self->sleepCalendar = calendarMS();
    self->sleepSeen = true;
    self->publishState(device != nullptr ? "sleep: controller noted" : "sleep: no controller before the sleep");
    return kIOReturnSuccess;
}

IOReturn BTWakeGate::willPowerOnAction(OSObject *owner, void *arg0, void *arg1, void *arg2, void *arg3)
{
    (void)arg0; (void)arg1; (void)arg2; (void)arg3;
    BTWakeGate *self = OSDynamicCast(BTWakeGate, owner);
    if (self == nullptr || self->stopping || !self->sleepSeen)
        return kIOReturnSuccess;
    self->sleepSeen = false;
    self->following = true;
    self->wakeUptime = uptimeMS();
    self->wakeCount++;
    self->keptOld = false;
    self->goneAt = kNever;
    self->readyAt = kNever;
    self->resetAt = kNever;
    self->sleptAtDecision = kNever;
    self->holdMS = 0;
    self->holdEnd = nullptr;
    self->firmware = -1;
    self->resetReturned = false;
    self->resetResult = kIOReturnSuccess;
    self->deadline = self->gateMaxMS;
    // The gate is closed by the first timer tick, on our work loop, so that the power management
    // thread never waits for the registry locks that adjustBusy() takes; a bluetoothd started during
    // the wake calls IOKitWaitQuiet() only after the controller has gone away (+0.3 s or later).
    self->closePending = self->enabled && self->oldDevice != nullptr;
    self->publishState(self->closePending ? "holding: waiting for the controller" : "following the wake, no gate");
    if (self->timer != nullptr)
        self->timer->setTimeoutMS(1);
    return kIOReturnSuccess;
}

IOReturn BTWakeGate::stopAction(OSObject *owner, void *arg0, void *arg1, void *arg2, void *arg3)
{
    (void)arg0; (void)arg1; (void)arg2; (void)arg3;
    BTWakeGate *self = OSDynamicCast(BTWakeGate, owner);
    if (self == nullptr)
        return kIOReturnSuccess;
    self->stopping = true;
    self->closePending = false;
    if (self->following)
        self->finishWake("the driver stopped");
    self->openGate("the driver stopped");
    self->sleepSeen = false;
    return kIOReturnSuccess;
}

void BTWakeGate::timerAction(OSObject *owner, IOTimerEventSource *sender)
{
    (void)sender;
    BTWakeGate *self = OSDynamicCast(BTWakeGate, owner);
    if (self != nullptr)
        self->onTick();
}

void BTWakeGate::onTick()
{
    if (!following || stopping)
        return;
    UInt64 now = uptimeMS();
    UInt64 t = now - wakeUptime;
    if (closePending) {
        closePending = false;
        closeGate();
    }

    // The controller from before the sleep: destroyed after the wake (mode A) or not (mode B)?
    if (oldDevice != nullptr && goneAt == kNever && oldDevice->isInactive())
        goneAt = t;

    // A new controller that has finished matching (the firmware is loaded) and is not busy.
    if (readyAt == kNever) {
        bool quiet = false;
        int loaded = -1;
        IOService *device = copyController(true, &quiet, &loaded);
        if (device != nullptr) {
            if (quiet) {
                readyAt = t;
                firmware = loaded;
                if (holding)
                    openGate(loaded == 1 ? "controller ready" : "controller matched, no FirmwareLoaded");
            }
            device->release();
        }
    }

    // Mode B: the controller from before the sleep is still there after SurvivorDelayMs.
    if (holding && readyAt == kNever && resetAt == kNever && t >= survivorDelayMS &&
        oldDevice != nullptr && !oldDevice->isInactive()) {
        sleptAtDecision = sleptMS(now);
        if (sleptAtDecision < minSleepMS) {
            keptOld = true;
            openGate("old controller kept, no real sleep");
        } else if (!resetSurvivor) {
            keptOld = true;
            openGate("old controller kept after a sleep, ResetSurvivor off");
        } else if (!resetAllowed(now)) {
            keptOld = true;
            openGate("old controller kept after a sleep, reset limit reached");
        } else {
            sendReset(t, now);
        }
    }

    if (holding && t >= deadline)
        openGate(resetAt != kNever ? "no new controller after the reset, GateMaxMs" : "no new controller, GateMaxMs");

    // Settled: the new controller is ready, or the old one is kept (it is still there).
    bool oldThere = oldDevice != nullptr && !oldDevice->isInactive();
    bool settled = readyAt != kNever || (keptOld && oldThere);
    if ((!holding && !resetInFlight && t >= kMinRecordMS && settled) || t >= kFollowMaxMS) {
        finishWake(nullptr);
        return;
    }
    if (timer != nullptr)
        timer->setTimeoutMS((holding || (readyAt == kNever && t < kFastPhaseMS)) ? kPollFastMS : kPollSlowMS);
}

// Writes the wake down (LastWake, History) and ends following it.
void BTWakeGate::finishWake(const char *why)
{
    if (!following)
        return;
    UInt64 now = uptimeMS();
    closePending = false;
    if (holding)
        openGate(why != nullptr ? why : "end of the wake record");
    following = false;
    if (timer != nullptr)
        timer->cancelTimeout();

    char when[40];
    UInt64 t = now - wakeUptime;
    UInt64 calendar = calendarMS();
    formatUTC((calendar > t ? calendar - t : 0) / 1000ULL, when, sizeof(when));

    char slept[24];
    formatDuration(sleptMS(now), slept, sizeof(slept));

    char old[48];
    char buf[24];
    if (oldDevice == nullptr) {
        snprintf(old, sizeof(old), "no controller before the sleep");
    } else if (goneAt != kNever) {
        formatOffset(goneAt, buf, sizeof(buf));
        snprintf(old, sizeof(old), "old gone %s", buf);
    } else if (oldDevice->isInactive()) {
        snprintf(old, sizeof(old), "old gone after the record");
    } else {
        snprintf(old, sizeof(old), "old kept");
    }

    char reset[64];
    if (resetAt == kNever) {
        snprintf(reset, sizeof(reset), "reset no");
    } else {
        formatOffset(resetAt, buf, sizeof(buf));
        if (resetReturned)
            snprintf(reset, sizeof(reset), "reset %s returned 0x%08x", buf, static_cast<UInt32>(resetResult));
        else
            snprintf(reset, sizeof(reset), "reset %s no answer yet", buf);
    }

    char ready[64];
    if (readyAt == kNever && keptOld && oldDevice != nullptr && !oldDevice->isInactive()) {
        snprintf(ready, sizeof(ready), "no new controller");
    } else if (readyAt == kNever) {
        snprintf(ready, sizeof(ready), "new controller not ready by %llu s", t / 1000ULL);
    } else {
        formatOffset(readyAt, buf, sizeof(buf));
        snprintf(ready, sizeof(ready), "new ready %s firmware %s", buf,
                 firmware == 1 ? "loaded" : (firmware == 0 ? "NOT loaded" : "unknown"));
    }

    char gate[128];
    if (holdEnd == nullptr) {
        snprintf(gate, sizeof(gate), "%s", enabled ? "no gate" : "no gate, Enabled off");
    } else {
        char held[24];
        formatOffset(holdMS, held, sizeof(held));
        snprintf(gate, sizeof(gate), "gate %s: %s", held + 1, holdEnd);
    }

    char line[400];
    snprintf(line, sizeof(line), "#%u %s | slept %s | %s | %s | %s | %s",
             wakeCount, when, slept, old, reset, ready, gate);
    addHistory(line);
    setProperty("LastWake", line);
    IOLog("BTWakeGate: %s\n", line);

    forgetOldDevice();
    publishCounters();
    publishState(why != nullptr ? why : "idle");
}

void BTWakeGate::publishState(const char *state)
{
    setProperty("State", state);
}

void BTWakeGate::publishCounters()
{
    setProperty("Wakes", wakeCount, 32);
    setProperty("Holds", holdCount, 32);
    setProperty("Resets", resetCount, 32);
}

void BTWakeGate::addHistory(const char *line)
{
    if (history == nullptr)
        return;
    OSString *string = OSString::withCString(line);
    if (string != nullptr) {
        history->setObject(string);
        string->release();
    }
    while (history->getCount() > kHistoryMax)
        history->removeObject(0);
    // Publish a copy, so that ioreg never sees an array that is being changed.
    OSArray *copy = OSArray::withArray(history);
    if (copy != nullptr) {
        setProperty("History", copy);
        copy->release();
    }
}
