// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#include "../core/realtime_synth.h"
#import <AudioToolbox/AudioToolbox.h>
#import <AVFoundation/AVFoundation.h>
#include "../core/d_printf.h"
#include "../core/minivulkan.h"

static uint32_t actual_sampling_rate;

@interface SynthAU: AUAudioUnit {
    AUAudioUnitBusArray* m_busses;
}
@end

@implementation SynthAU

    - (instancetype)initWithComponentDescription: (AudioComponentDescription) componentDescription
                    error:                        (NSError**)                 outError
    {
        self = [super initWithComponentDescription: componentDescription
                      options: 0
                      error: outError];
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

    - (BOOL)allocateRenderResourcesAndReturnError: (NSError**)out_error
    {
        if (![super allocateRenderResourcesAndReturnError: out_error])
            return NO;

        AVAudioFormat* outputFormat = m_busses[0].format;

        if (outputFormat.channelCount != 2) {
            d_printf("Unsupported output channel count %u\n", outputFormat.channelCount);
            return NO;
        }

        if (outputFormat.interleaved) {
            d_printf("Output channels are interleaved\n");
            return NO;
        }

        if ( ! outputFormat.standard) {
            d_printf("Output channel format is not float\n");
            return NO;
        }

        actual_sampling_rate = static_cast<uint32_t>(outputFormat.sampleRate);
        d_printf("Sampling rate %u Hz\n", actual_sampling_rate);

        return YES;
    }

    - (AUInternalRenderBlock)internalRenderBlock
    {
        return ^AUAudioUnitStatus(AudioUnitRenderActionFlags* action_flags,
                                  const AudioTimeStamp*       timestamp,
                                  AUAudioFrameCount           num_frames,
                                  NSInteger                   output_bus_number,
                                  AudioBufferList*            output_data,
                                  const AURenderEvent*        realtime_event_list_head,
                                  AURenderPullInputBlock      pull_input_block)
        {
            assert(output_bus_number == 0);

            if (output_data->mNumberBuffers != 2)
                return noErr;

            const AURenderEvent* event = realtime_event_list_head;
            while (event) {
                switch (event->head.eventType) {

                    case AURenderEventMIDI:
                        // TODO handle MIDI event
                        break;

                    case AURenderEventMIDISysEx:
                        // TODO handle MIDI system exclusive event
                        break;

                    default:
                        break;
                }
                event = event->head.next;
            }

            render_audio_buffer(actual_sampling_rate,
                                num_frames,
                                static_cast<float*>(output_data->mBuffers[0].mData),
                                static_cast<float*>(output_data->mBuffers[1].mData));

            output_data->mBuffers[0].mDataByteSize = num_frames * sizeof(float);
            output_data->mBuffers[1].mDataByteSize = num_frames * sizeof(float);

            return noErr;
        };
    }

@end

bool init_real_time_synth_os()
{
    static const AudioComponentDescription synth_desc = {
        kAudioUnitType_MusicDevice,
        kAudioUnitSubType_MIDISynth,
        'drgn',
        0,
        0
    };

    [SynthAU registerSubclass:       [SynthAU class]
             asComponentDescription: synth_desc
             name:                   [[NSString alloc] initWithCString: app_name
                                                       encoding: NSASCIIStringEncoding]
             version:                1];

    AVAudioEngine* const engine = [[AVAudioEngine alloc] init];
    if ( ! engine) {
        d_printf("Failed to create AVAudioEngine\n");
        return true;
    }

    static bool failed;
    failed = false;

    [AVAudioUnit instantiateWithComponentDescription: synth_desc
                 options: 0
                 completionHandler:
        ^(AVAudioUnit * _Nullable audio_unit, NSError * _Nullable error)
        {
            if (error) {
                d_printf("Failed instantiate AVAudioUnit\n");
                failed = true;
                return;
            }

            [engine attachNode: audio_unit];

            AVAudioNode* output_node = engine.outputNode;
            if ( ! output_node) {
                d_printf("Failed to attach output node\n");
                failed = true;
                return;
            }

            [engine connect: audio_unit to: output_node format: nil];

            NSError* start_error = nil;
            [engine startAndReturnError: &start_error];
            if (start_error) {
                d_printf("Failed to start AVAudioEngine\n");
                failed = true;
                return;
            }
        }
    ];

    return ! failed;
}
