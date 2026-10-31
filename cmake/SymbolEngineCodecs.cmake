include_guard(GLOBAL)

include(FetchContent)

# The codecs for the files module, each pinned by checksum: Xiph.Org's libogg, libvorbis, libFLAC, libopus, opusfile and
# libopusenc, under the BSD license whose notices a product includes with its others, and minimp3, in the public
# domain. Their own builds aren't run; their library sources are compiled here, with their settings given as
# definitions and their processor-specific code left out, so they build alike everywhere, and their objects go inside
# the files module's library.

FetchContent_Declare(symbolengine_ogg
    URL https://github.com/xiph/ogg/archive/refs/tags/v1.3.6.tar.gz
    URL_HASH SHA256=95b643da661155d79db9de2fca55daed3a8d491039829def246aacb3d9201c81
    SOURCE_SUBDIR fetched-only)
# libvorbis from after its last release, 1.3.7 of 2020, for the fixes made since, among them a use-after-free in seeking.
FetchContent_Declare(symbolengine_vorbis
    URL https://github.com/xiph/vorbis/archive/1b75110b5a2754ba1931d82dd83cb822b266a21d.tar.gz
    URL_HASH SHA256=bd0f08c8a63debdce27dd71de7d953f9c92e410d1cfe263220bcb49fff2ab749
    SOURCE_SUBDIR fetched-only)
FetchContent_Declare(symbolengine_flac
    URL https://github.com/xiph/flac/archive/refs/tags/1.5.0.tar.gz
    URL_HASH SHA256=aea54ed186ad07a34750399cb27fc216a2b62d0ffcd6dc2e3064a3518c3146f8
    SOURCE_SUBDIR fetched-only)
FetchContent_Declare(symbolengine_opus
    URL https://github.com/xiph/opus/archive/refs/tags/v1.5.2.tar.gz
    URL_HASH SHA256=9480e329e989f70d69886ded470c7f8cfe6c0667cc4196d4837ac9e668fb7404
    SOURCE_SUBDIR fetched-only)
FetchContent_Declare(symbolengine_opusfile
    URL https://github.com/xiph/opusfile/archive/refs/tags/v0.12.tar.gz
    URL_HASH SHA256=a20a1dff1cdf0719d1e995112915e9966debf1470ee26bb31b2f510ccf00ef40
    SOURCE_SUBDIR fetched-only)
FetchContent_Declare(symbolengine_opusenc
    URL https://github.com/xiph/libopusenc/archive/refs/tags/v0.3.tar.gz
    URL_HASH SHA256=5637474cdf4ff693977fe74039049349fef7b65e3a94ad8bde51ecfcd1e94a8e
    SOURCE_SUBDIR fetched-only)
FetchContent_Declare(symbolengine_minimp3
    URL https://github.com/lieff/minimp3/archive/ea99364f61c14656440e8d77e9c233ccf3124633.tar.gz
    URL_HASH SHA256=5628166eb82a9bb581317918a334c317a2c0a30278bb14a20381307976768f34
    SOURCE_SUBDIR fetched-only)
FetchContent_MakeAvailable(symbolengine_ogg symbolengine_vorbis symbolengine_flac symbolengine_opus symbolengine_opusfile
                           symbolengine_opusenc symbolengine_minimp3)

set(ogg ${symbolengine_ogg_SOURCE_DIR})
set(vorbis ${symbolengine_vorbis_SOURCE_DIR})
set(flac ${symbolengine_flac_SOURCE_DIR})
set(opus ${symbolengine_opus_SOURCE_DIR})
set(opusfile ${symbolengine_opusfile_SOURCE_DIR})
set(opusenc ${symbolengine_opusenc_SOURCE_DIR})
set(minimp3 ${symbolengine_minimp3_SOURCE_DIR})

# libogg's sizes, which its own build works out into config_types.h.
set(codec_generated ${CMAKE_CURRENT_BINARY_DIR}/codecs)
file(WRITE ${codec_generated}/ogg/config_types.h.new [=[
#ifndef __CONFIG_TYPES_H__
#define __CONFIG_TYPES_H__
#include <stdint.h>
typedef int16_t ogg_int16_t;
typedef uint16_t ogg_uint16_t;
typedef int32_t ogg_int32_t;
typedef uint32_t ogg_uint32_t;
typedef int64_t ogg_int64_t;
typedef uint64_t ogg_uint64_t;
#endif
]=])
file(COPY_FILE ${codec_generated}/ogg/config_types.h.new ${codec_generated}/ogg/config_types.h ONLY_IF_DIFFERENT)

# The libraries' public headers, which the files module's own sources include too.
set(codec_includes
    ${codec_generated} ${ogg}/include ${vorbis}/include ${flac}/include ${opus}/include ${opusfile}/include ${opusenc}/include
    ${minimp3})

# minimp3 is a header, whose code one source file compiles, decoding to floats.
file(WRITE ${codec_generated}/minimp3.c.new "#define MINIMP3_IMPLEMENTATION\n#define MINIMP3_FLOAT_OUTPUT\n#include \"minimp3_ex.h\"\n")
file(COPY_FILE ${codec_generated}/minimp3.c.new ${codec_generated}/minimp3.c ONLY_IF_DIFFERENT)
set(minimp3_sources ${codec_generated}/minimp3.c)

set(ogg_sources ${ogg}/src/bitwise.c ${ogg}/src/framing.c)

set(vorbis_sources)
foreach(file mdct smallft block envelope window lsp lpc analysis synthesis psy info floor1 floor0 res0 mapping0 registry
             codebook sharedbook lookup bitrate vorbisfile)
    list(APPEND vorbis_sources ${vorbis}/lib/${file}.c)
endforeach()

# Its encoder sets up 5.1's LFE residue once for each block size, and the second time loses the first; a copy of
# vorbisenc.c lets the first go before it does, until libvorbis has the fix (Docs/Upstream/libvorbis-51-residue-leak.md).
file(READ ${vorbis}/lib/vorbisenc.c vorbisenc)
set(vorbis_residue "  vorbis_info_residue0 *r=ci->residue_param[number]=\n    _ogg_malloc(sizeof(*r));")
string(FIND "${vorbisenc}" "${vorbis_residue}" found)
if(found EQUAL -1)
    message(FATAL_ERROR "libvorbis' vorbisenc.c no longer sets up residues as SymbolEngine's copy of it expects")
endif()
string(REPLACE "${vorbis_residue}"
    "  vorbis_info_residue0 *r;\n  if(ci->residue_param[number])_ogg_free(ci->residue_param[number]);\n  r=ci->residue_param[number]=\n    _ogg_malloc(sizeof(*r));"
    vorbisenc "${vorbisenc}")
file(WRITE ${codec_generated}/vorbisenc.c.new "${vorbisenc}")
file(COPY_FILE ${codec_generated}/vorbisenc.c.new ${codec_generated}/vorbisenc.c ONLY_IF_DIFFERENT)
list(APPEND vorbis_sources ${codec_generated}/vorbisenc.c)

set(flac_sources)
foreach(file bitmath bitreader bitwriter cpu crc fixed float format lpc md5 memory metadata_iterators metadata_object
             stream_decoder stream_encoder stream_encoder_framing window)
    list(APPEND flac_sources ${flac}/src/libFLAC/${file}.c)
endforeach()
if(WIN32)
    list(APPEND flac_sources ${flac}/src/share/win_utf8_io/win_utf8_io.c)
endif()

# Opus's sources, as its opus_sources.mk, celt_sources.mk and silk_sources.mk list them for a floating-point build.
set(opus_sources)
foreach(file opus opus_decoder opus_encoder extensions opus_multistream opus_multistream_encoder opus_multistream_decoder
             repacketizer opus_projection_encoder opus_projection_decoder mapping_matrix analysis mlp mlp_data)
    list(APPEND opus_sources ${opus}/src/${file}.c)
endforeach()
foreach(file bands celt celt_encoder celt_decoder cwrs entcode entdec entenc kiss_fft laplace mathops mdct modes pitch
             celt_lpc quant_bands rate vq)
    list(APPEND opus_sources ${opus}/celt/${file}.c)
endforeach()
foreach(file CNG code_signs init_decoder decode_core decode_frame decode_parameters decode_indices decode_pulses
             decoder_set_fs dec_API enc_API encode_indices encode_pulses gain_quant interpolate LP_variable_cutoff
             NLSF_decode NSQ NSQ_del_dec PLC shell_coder tables_gain tables_LTP tables_NLSF_CB_NB_MB tables_NLSF_CB_WB
             tables_other tables_pitch_lag tables_pulses_per_block VAD control_audio_bandwidth quant_LTP_gains VQ_WMat_EC
             HP_variable_cutoff NLSF_encode NLSF_VQ NLSF_unpack NLSF_del_dec_quant process_NLSFs stereo_LR_to_MS
             stereo_MS_to_LR check_control_input control_SNR init_encoder control_codec A2NLSF ana_filt_bank_1
             biquad_alt bwexpander_32 bwexpander debug decode_pitch inner_prod_aligned lin2log log2lin
             LPC_analysis_filter LPC_inv_pred_gain table_LSF_cos NLSF2A NLSF_stabilize NLSF_VQ_weights_laroia
             pitch_est_tables resampler resampler_down2_3 resampler_down2 resampler_private_AR2
             resampler_private_down_FIR resampler_private_IIR_FIR resampler_private_up2_HQ resampler_rom sigm_Q15 sort
             sum_sqr_shift stereo_decode_pred stereo_encode_pred stereo_find_predictor stereo_quant_pred LPC_fit)
    list(APPEND opus_sources ${opus}/silk/${file}.c)
endforeach()
foreach(file apply_sine_window_FLP corrMatrix_FLP encode_frame_FLP find_LPC_FLP find_LTP_FLP find_pitch_lags_FLP
             find_pred_coefs_FLP LPC_analysis_filter_FLP LTP_analysis_filter_FLP LTP_scale_ctrl_FLP
             noise_shape_analysis_FLP process_gains_FLP regularize_correlations_FLP residual_energy_FLP
             warped_autocorrelation_FLP wrappers_FLP autocorrelation_FLP burg_modified_FLP bwexpander_FLP energy_FLP
             inner_product_FLP k2a_FLP LPC_inv_pred_gain_FLP pitch_analysis_core_FLP scale_copy_vector_FLP
             scale_vector_FLP schur_FLP sort_FLP)
    list(APPEND opus_sources ${opus}/silk/float/${file}.c)
endforeach()

# Opening a chained file, it keeps a link's tags and then goes looking for that link's audio, and on finding none forgets
# the tags; a copy of opusfile.c lets them go, until opusfile has the fix (Docs/Upstream/opusfile-chained-tags-leak.md).
file(READ ${opusfile}/src/opusfile.c opusfile_c)
set(opusfile_link_audio "    ret=op_find_initial_pcm_offset(_of,links+nlinks,NULL);\n    if(OP_UNLIKELY(ret<0))return ret;\n    links[nlinks].pcm_file_offset=total_duration;")
string(FIND "${opusfile_c}" "${opusfile_link_audio}" found)
if(found EQUAL -1)
    message(FATAL_ERROR "opusfile's opusfile.c no longer opens a chained file's links as SymbolEngine's copy of it expects")
endif()
string(REPLACE "${opusfile_link_audio}"
    "    ret=op_find_initial_pcm_offset(_of,links+nlinks,NULL);\n    if(OP_UNLIKELY(ret<0)){\n      opus_tags_clear(&links[nlinks].tags);\n      return ret;\n    }\n    links[nlinks].pcm_file_offset=total_duration;"
    opusfile_c "${opusfile_c}")
file(WRITE ${codec_generated}/opusfile.c.new "${opusfile_c}")
file(COPY_FILE ${codec_generated}/opusfile.c.new ${codec_generated}/opusfile.c ONLY_IF_DIFFERENT)

set(opusfile_sources ${opusfile}/src/info.c ${opusfile}/src/internal.c ${codec_generated}/opusfile.c ${opusfile}/src/stream.c)

set(opusenc_sources)
foreach(file ogg_packer opus_header opusenc picture resample unicode_support)
    list(APPEND opusenc_sources ${opusenc}/src/${file}.c)
endforeach()

add_library(symbolengine_codecs OBJECT
    ${ogg_sources} ${vorbis_sources} ${flac_sources} ${opus_sources} ${opusfile_sources} ${opusenc_sources} ${minimp3_sources})
set_target_properties(symbolengine_codecs PROPERTIES
    POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden C_STANDARD 99)
target_include_directories(symbolengine_codecs SYSTEM PRIVATE ${codec_includes})

# Each library's own private headers, apart, as some share names, such as Vorbis' and Opus' mdct.h.
set_source_files_properties(${vorbis_sources} PROPERTIES INCLUDE_DIRECTORIES "${vorbis}/lib")
set_source_files_properties(${flac_sources} PROPERTIES INCLUDE_DIRECTORIES "${flac}/src/libFLAC/include")
set_source_files_properties(${codec_generated}/opusfile.c PROPERTIES INCLUDE_DIRECTORIES "${opusfile}/src")
set_source_files_properties(${opus_sources} PROPERTIES INCLUDE_DIRECTORIES "${opus};${opus}/celt;${opus}/silk;${opus}/silk/float;${opus}/src")

# Their code isn't held to SymbolEngine's warnings.
if(MSVC)
    target_compile_options(symbolengine_codecs PRIVATE /w)
    target_compile_definitions(symbolengine_codecs PRIVATE _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)
else()
    target_compile_options(symbolengine_codecs PRIVATE -w)
endif()

# Every function stays hidden inside whatever links it, as SymbolEngine's own do, which Opus, libopusenc and opusfile
# would otherwise make visible on GCC and Clang; and the codecs' own debugging output is left out of every build.
target_compile_definitions(symbolengine_codecs PRIVATE FLAC__NO_DLL OPUS_EXPORT= OPE_EXPORT= NDEBUG)
if(WIN32)
    set_source_files_properties(${flac_sources} PROPERTIES COMPILE_DEFINITIONS "PACKAGE_VERSION=\"1.5.0\";HAVE_LROUND=1;HAVE_STDINT_H=1;FLAC__HAS_OGG=0")
else()
    set_source_files_properties(${flac_sources} PROPERTIES COMPILE_DEFINITIONS "PACKAGE_VERSION=\"1.5.0\";HAVE_LROUND=1;HAVE_STDINT_H=1;FLAC__HAS_OGG=0;HAVE_FSEEKO=1")
endif()
set_source_files_properties(${opus_sources} PROPERTIES COMPILE_DEFINITIONS "OPUS_BUILD;USE_ALLOCA;HAVE_LRINT;HAVE_LRINTF")
set_source_files_properties(${opusfile_sources} PROPERTIES COMPILE_DEFINITIONS "OP_HAVE_LRINTF")

# opusfile's header makes its functions visible on GCC and Clang when it takes the compiler for GCC 4 or later; its
# sources are given a header first that says no, as CMake's definitions can't take a macro with arguments.
if(NOT MSVC)
    file(WRITE ${codec_generated}/opusfile_hidden.h.new "#define OP_GNUC_PREREQ(_maj, _min) 0\n")
    file(COPY_FILE ${codec_generated}/opusfile_hidden.h.new ${codec_generated}/opusfile_hidden.h ONLY_IF_DIFFERENT)
    set_source_files_properties(${opusfile_sources} PROPERTIES COMPILE_OPTIONS "-include;${codec_generated}/opusfile_hidden.h")
endif()
set_source_files_properties(${opusenc_sources} PROPERTIES COMPILE_DEFINITIONS
    "OPE_BUILD;OUTSIDE_SPEEX;RANDOM_PREFIX=libopusenc;FLOATING_POINT;RESAMPLE_FULL_SINC_TABLE;PACKAGE_NAME=\"libopusenc\";PACKAGE_VERSION=\"0.3\"")
