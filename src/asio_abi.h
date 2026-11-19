// Low Latency ASIO for UMC22 - ASIO interface re-implemented at the ABI level (x64), without the Steinberg SDK
// Copyright (C) 2026 Tohzzicklao
// SPDX-License-Identifier: GPL-3.0-or-later
// Part of the single translation unit built from umc22asio.cpp: include it only from there.
#pragma once

// ============================================================ ABI ASIO
typedef long ASIOBool;
typedef long ASIOError;
typedef double ASIOSampleRate;
typedef long ASIOSampleType;
struct ASIOSamples { unsigned long hi, lo; };
typedef ASIOSamples ASIOTimeStamp;
enum {
    ASE_OK = 0, ASE_SUCCESS = 0x3f4847a0, ASE_NotPresent = -1000, ASE_HWMalfunction,
    ASE_InvalidParameter, ASE_InvalidMode, ASE_SPNotAdvancing, ASE_NoClock, ASE_NoMemory
};
enum { ASIOSTInt32LSB = 18 };
enum { kAsioSelectorSupported = 1, kAsioEngineVersion = 2, kAsioResetRequest = 3 };
struct ASIOClockSource { long index, associatedChannel, associatedGroup; ASIOBool isCurrentSource; char name[32]; };
struct ASIOChannelInfo { long channel; ASIOBool isInput, isActive; long channelGroup; ASIOSampleType type; char name[32]; };
struct ASIOBufferInfo { ASIOBool isInput; long channelNum; void* buffers[2]; };
struct ASIOTime;
struct ASIOCallbacks {
    void (*bufferSwitch)(long doubleBufferIndex, ASIOBool directProcess);
    void (*sampleRateDidChange)(ASIOSampleRate sRate);
    long (*asioMessage)(long selector, long value, void* message, double* opt);
    ASIOTime* (*bufferSwitchTimeInfo)(ASIOTime* params, long doubleBufferIndex, ASIOBool directProcess);
};

struct IASIO : public IUnknown {
    virtual ASIOBool init(void* sysHandle) = 0;
    virtual void getDriverName(char* name) = 0;
    virtual long getDriverVersion() = 0;
    virtual void getErrorMessage(char* string) = 0;
    virtual ASIOError start() = 0;
    virtual ASIOError stop() = 0;
    virtual ASIOError getChannels(long* numInputChannels, long* numOutputChannels) = 0;
    virtual ASIOError getLatencies(long* inputLatency, long* outputLatency) = 0;
    virtual ASIOError getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) = 0;
    virtual ASIOError canSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError getSampleRate(ASIOSampleRate* sampleRate) = 0;
    virtual ASIOError setSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError getClockSources(ASIOClockSource* clocks, long* numSources) = 0;
    virtual ASIOError setClockSource(long reference) = 0;
    virtual ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) = 0;
    virtual ASIOError getChannelInfo(ASIOChannelInfo* info) = 0;
    virtual ASIOError createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize, ASIOCallbacks* callbacks) = 0;
    virtual ASIOError disposeBuffers() = 0;
    virtual ASIOError controlPanel() = 0;
    virtual ASIOError future(long selector, void* opt) = 0;
    virtual ASIOError outputReady() = 0;
};
