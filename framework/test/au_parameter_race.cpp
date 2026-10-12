// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The AU as a host runs it, in this process: automation on one thread
// (AudioUnitSetParameter) while state loads on another (ClassInfo), and every
// state load makes the plugin rescan its parameters' values. A host may do
// both at once. Under the tsan preset, ThreadSanitizer reports any access the
// two make to the same memory without ordering, whether or not they collide
// in this run.
//
//   au-parameter-race <Product.component>
//
// The component is registered in this process from its own bundle, so
// nothing is installed and the system's AU cache is not involved.
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-56s %s\n", what, ok ? "ok" : "FAIL");
    std::fflush(stdout);
    if (!ok) ++failures;
}

// "aufx" in the bundle's Info.plist, as the four-character code it names.
OSType fourCC(CFDictionaryRef entry, CFStringRef key) {
    const auto text = static_cast<CFStringRef>(CFDictionaryGetValue(entry, key));
    char c[8] = {};
    if (!text || CFGetTypeID(text) != CFStringGetTypeID() ||
        !CFStringGetCString(text, c, sizeof(c), kCFStringEncodingMacRoman) || std::strlen(c) != 4)
        return 0;
    return (OSType(uint8_t(c[0])) << 24) | (OSType(uint8_t(c[1])) << 16) | (OSType(uint8_t(c[2])) << 8) |
           OSType(uint8_t(c[3]));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: au-parameter-race <Product.component>\n");
        return 2;
    }
    std::printf("AU parameter race (%s)\n", argv[1]);

    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(argv[1]),
                                                           static_cast<CFIndex>(std::strlen(argv[1])), true);
    CFBundleRef bundle = url ? CFBundleCreate(nullptr, url) : nullptr;
    check(bundle != nullptr, "the bundle opens");
    if (!bundle) return 1;

    const auto components =
        static_cast<CFArrayRef>(CFBundleGetValueForInfoDictionaryKey(bundle, CFSTR("AudioComponents")));
    const auto entry = components && CFGetTypeID(components) == CFArrayGetTypeID() && CFArrayGetCount(components) > 0
                           ? static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(components, 0))
                           : nullptr;
    check(entry != nullptr, "Info.plist lists an audio component");
    if (!entry) return 1;

    AudioComponentDescription desc{};
    desc.componentType = fourCC(entry, CFSTR("type"));
    desc.componentSubType = fourCC(entry, CFSTR("subtype"));
    desc.componentManufacturer = fourCC(entry, CFSTR("manufacturer"));
    const auto factoryName = static_cast<CFStringRef>(CFDictionaryGetValue(entry, CFSTR("factoryFunction")));
    auto factory = factoryName ? reinterpret_cast<AudioComponentFactoryFunction>(
                                     CFBundleGetFunctionPointerForName(bundle, factoryName))
                               : nullptr;
    check(desc.componentType && factory, "its factory function loads");
    if (!factory) return 1;

    AudioComponent component = AudioComponentRegister(&desc, CFSTR("Supple: race probe"), 1, factory);
    AudioComponentInstance unit = nullptr;
    check(component && AudioComponentInstanceNew(component, &unit) == noErr && unit, "an instance is made");
    if (!unit) return 1;
    check(AudioUnitInitialize(unit) == noErr, "it initialises");

    // A parameter to automate, and its range.
    UInt32 size = 0;
    AudioUnitGetPropertyInfo(unit, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, &size, nullptr);
    std::vector<AudioUnitParameterID> ids(size / sizeof(AudioUnitParameterID));
    if (!ids.empty())
        AudioUnitGetProperty(unit, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, ids.data(), &size);
    check(!ids.empty(), "it lists parameters");
    if (ids.empty()) return 1;
    AudioUnitParameterInfo info{};
    size = sizeof(info);
    AudioUnitGetProperty(unit, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, ids[0], &info, &size);
    if (info.cfNameString && (info.flags & kAudioUnitParameterFlag_CFNameRelease)) CFRelease(info.cfNameString);

    CFPropertyListRef state = nullptr;
    size = sizeof(state);
    check(AudioUnitGetProperty(unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &state, &size) ==
                  noErr &&
              state,
          "its state saves");
    if (!state) return 1;

    // Automation, as fast as it goes, until the loads are done.
    std::atomic<bool> stop{false};
    std::atomic<long> sets{0};
    std::thread automation([&] {
        for (long n = 0; !stop.load(std::memory_order_relaxed); ++n) {
            AudioUnitSetParameter(unit, ids[0], kAudioUnitScope_Global, 0, (n & 1) ? info.maxValue : info.minValue, 0);
            sets.fetch_add(1, std::memory_order_relaxed);
        }
    });
    // The first sets land before the first load, so the two overlap.
    while (sets.load() < 100) std::this_thread::yield();

    bool loaded = true;
    for (int i = 0; i < 200; ++i) {
        loaded = loaded && AudioUnitSetProperty(unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &state,
                                                sizeof(state)) == noErr;
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.0, true);  // the wrapper's main-thread work
    }
    stop = true;
    automation.join();
    check(loaded, "200 state loads, each a parameter rescan");
    std::printf("     %ld parameter sets alongside them\n", sets.load());
    check(sets.load() > 100, "automation ran throughout");

    CFRelease(state);
    AudioUnitUninitialize(unit);
    AudioComponentInstanceDispose(unit);
    CFRelease(bundle);
    CFRelease(url);
    std::printf("\n%d failed\n", failures);
    return failures ? 1 : 0;
}
