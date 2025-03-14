// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#include "../core/realtime_audio.h"
#import <AudioToolbox/AudioToolbox.h>
#import <AVFoundation/AVFoundation.h>
#include "../core/d_printf.h"
#include "../core/minivulkan.h"
#include "../core/vmath.h"

@interface SynthAU: AUAudioUnit {
    AUAudioUnitBusArray* m_busses;
}
@end

@implementation SynthAU

    - (instancetype)initWithComponentDescription: (AudioComponentDescription) componentDescription
                    error:                        (NSError**)                 outError
    {
        self = [super initWithComponentDescription: componentDescription
                      options:                      0
                      error:                        outError];
        if (self) {
            m_busses = nil;
        }
        return self;
    }

    - (AUAudioUnitBusArray*)outputBusses
    {
        if ( ! m_busses) {
            AVAudioFormat* outputFormat = [[AVAudioFormat alloc]
                                                initStandardFormatWithSampleRate: rt_sampling_rate
                                                channels: 2];

            if ( ! outputFormat) {
                d_printf("Failed to create AVAudioFormat\n");
                return m_busses;
            }

            AUAudioUnitBus* outputBus = [[AUAudioUnitBus alloc]
                                                initWithFormat: outputFormat
                                                error: nil];
            if ( ! outputFormat) {
                d_printf("Failed to create AVAudioUnitBus\n");
                return m_busses;
            }

            m_busses = [[AUAudioUnitBusArray alloc]
                                    initWithAudioUnit: self
                                    busType: AUAudioUnitBusTypeOutput
                                    busses: @[outputBus]];
            if ( ! m_busses)
                d_printf("Failed to create bus AUAudioUnitBusArray\n");
        }
        return m_busses;
    }

    - (AUInternalRenderBlock)internalRenderBlock
    {
        return ^AUAudioUnitStatus(AudioUnitRenderActionFlags* action_flags,
                                  const AudioTimeStamp*       timestamp,
                                  AUAudioFrameCount           num_frames,
                                  NSInteger                   outputBusNumber,
                                  AudioBufferList*            outputData,
                                  const AURenderEvent*        realtimeEventListHead,
                                  AURenderPullInputBlock      pullInputBlock)
        {
            if (outputData->mNumberBuffers != 2)
                return noErr;

            // TODO render

            float* const left_chan  = (float *)outputData->mBuffers[0].mData;
            float* const right_chan = (float *)outputData->mBuffers[1].mData;

            for (AUAudioFrameCount i = 0; i < num_frames; i++) {
                static float phase = 0;
                static const float frequency = 440.0f;

                left_chan[i]  = sinf(phase);
                right_chan[i] = sinf(phase + vmath::pi);
                phase += 2.0f * vmath::pi * frequency / 44100.0f;
                if (phase > 2.0f * vmath::pi)
                    phase -= 2.0f * vmath::pi;
            }

            outputData->mBuffers[0].mDataByteSize = num_frames * sizeof(float);
            outputData->mBuffers[1].mDataByteSize = num_frames * sizeof(float);

            return noErr;
        };
    }

@end

bool init_real_time_audio()
{
    static const AudioComponentDescription synth_desc = {
        kAudioUnitType_MusicDevice,
        kAudioUnitSubType_MIDISynth,
        'drgn',
        0,
        0
    };

    [SynthAU registerSubclass:       SynthAU.class
             asComponentDescription: synth_desc
             name:                   [[NSString alloc] initWithCString: app_name
                                                       encoding: NSASCIIStringEncoding]
             version:                1];

    AVAudioEngine* engine = [[AVAudioEngine alloc] init];
    if ( ! engine) {
        d_printf("Failed to create AVAudioEngine\n");
        return true;
    }

    [AVAudioUnit instantiateWithComponentDescription: synth_desc
                 options: 0
                 completionHandler:
        ^(AVAudioUnit * _Nullable audioUnit, NSError * _Nullable error)
        {
            if (error) {
                d_printf("Failed instantiate AVAudioUnit\n");
                return;
            }

            [engine attachNode: audioUnit];

            AVAudioNode* output_node = engine.outputNode;
            if ( ! output_node) {
                d_printf("Failed to attach output node\n");
                return;
            }

            [engine connect: audioUnit to: output_node format: nil];

            NSError *startError;
            [engine startAndReturnError: &startError];
            if (startError) {
                d_printf("Failed to start AVAudioEngine\n");
                return;
            }
        }
    ];

    return true;
}
