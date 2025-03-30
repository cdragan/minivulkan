// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#include "../core/realtime_synth.h"
#import <AudioToolbox/AudioToolbox.h>
#import <AVFoundation/AVFoundation.h>
#include "../core/d_printf.h"
#include "../core/minivulkan.h"

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
                                                initStandardFormatWithSampleRate: Synth::rt_sampling_rate
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

        const uint32_t sampling_rate = static_cast<uint32_t>(outputFormat.sampleRate);
        if (sampling_rate != Synth::rt_sampling_rate) {
            d_printf("OS changed sampling rate to %u Hz - unsupported\n", sampling_rate);
            return NO;
        }

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

            Synth::render_audio_buffer(num_frames,
                                       static_cast<float*>(output_data->mBuffers[0].mData),
                                       static_cast<float*>(output_data->mBuffers[1].mData));

            output_data->mBuffers[0].mDataByteSize = num_frames * sizeof(float);
            output_data->mBuffers[1].mDataByteSize = num_frames * sizeof(float);

            return noErr;
        };
    }

@end

static AVAudioNode* output_node;
static double       output_sample_rate;

namespace Synth {

bool init_synth_os()
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

            output_node = engine.outputNode;
            if ( ! output_node) {
                d_printf("Failed to attach output node\n");
                failed = true;
                return;
            }

            [engine connect: audio_unit to: output_node format: nil];

            output_sample_rate = [output_node outputFormatForBus: 0].sampleRate;

            NSError* start_error = nil;
            [engine startAndReturnError: &start_error];
            if (start_error) {
                d_printf("Failed to start AVAudioEngine\n");
                output_node = nullptr;
                failed = true;
                return;
            }
        }
    ];

    return ! failed;
}

uint64_t get_current_timestamp_ms()
{
    AVAudioTime* const last_render_time = output_node.lastRenderTime;

    static uint64_t saved_timestamp_ms;

    if (last_render_time) {
        const int64_t sample_frames = last_render_time.sampleTime;

        if (sample_frames >= 0)
            saved_timestamp_ms = static_cast<uint64_t>(sample_frames) * 1000U / static_cast<uint64_t>(output_sample_rate);
    }

    return saved_timestamp_ms;
}

} // namespace Synth
