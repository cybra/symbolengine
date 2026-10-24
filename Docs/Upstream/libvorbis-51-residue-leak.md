# libvorbis: the 5.1 encoder leaks the LFE's residue

A comment for Xiph.Org's issue [#2357, "Memory leak in encoding code in case of six channels"](https://gitlab.xiph.org/xiph/vorbis/-/issues/2357), on their GitLab, where libvorbis is maintained. Xiph's GitHub repository is a mirror: its pull requests are closed without being merged. The same leak was reported there as [xiph/vorbis#91](https://github.com/xiph/vorbis/issues/91) in 2022 and never answered. The comment below gives the cause and a fix, which neither report has; a merge request on GitLab with the same change would do as well. SymbolEngine builds its own copy of `vorbisenc.c` with the change meanwhile (`cmake/SymbolEngineCodecs.cmake`), and goes back to libvorbis' own once the fix is in.

## Comment

The leak comes from the 5.1 mapping templates sharing the LFE's residue between the two block sizes.

`_map_nominal_51` and `_map_nominal_51u` in `lib/modes/residue_44p51.h` give the short-block mapping residues 0 and 2 and the long-block mapping residues 1 and 2, so residue 2 belongs to both. `vorbis_encode_map_n_res_setup` calls `vorbis_encode_residue_setup` for every submap of each mapping, which sets residue 2 up twice, and the second call's `ci->residue_param[number]=_ogg_malloc(sizeof(*r))` overwrites the first call's pointer. One `vorbis_info_residue0`, 2,848 bytes on a 64-bit system, is lost each time an encoder is set up from `ve_setup_44_51`: six channels at 40 to 70 kHz, at any quality or bitrate. `vorbis_info_clear` frees only the second.

It is also GitHub's xiph/vorbis#91: its 62,656 bytes in 22 objects are 22 of these, one for each six-channel configuration `vorbis_test` encodes in that range.

Freeing the earlier one before setting the residue up again fixes it:

```diff
--- a/lib/vorbisenc.c
+++ b/lib/vorbisenc.c
@@ -460,7 +460,9 @@
   codec_setup_info *ci=vi->codec_setup;
   int i;
 
-  vorbis_info_residue0 *r=ci->residue_param[number]=
+  vorbis_info_residue0 *r;
+  if(ci->residue_param[number])_ogg_free(ci->residue_param[number]);
+  r=ci->residue_param[number]=
     _ogg_malloc(sizeof(*r));
 
   memcpy(r,res->res,sizeof(*r));
```

The second setup already decides what the residue holds, so the encoded output is unchanged. Checked on `main` at 1b75110 with AddressSanitizer's leak checking (Homebrew clang 22.1.8, macOS on arm64):

- `vorbis_test`: before, 62,656 bytes leaked in 22 allocations and exit status 1; after, no leaks and exit status 0, with every stream reading back as ok.
- A program encoding one second of six-channel audio at 44.1 kHz quality 0, at 48 kHz qualities 0.6 and 1.0, and at 48 kHz managed at 128 kbps: before, one 2,848-byte leak per encoder; after, none, and each stream's bytes identical to before.

With this in, the encode fuzzer from !45 needn't skip six channels.

## Follow-up on the issue

Timothy B. Terriberry answered that the fix stops the leak without changing behavior, but that the behavior may not be the one wanted: a residue set up a second time silently replaces the first, though the two may differ. Our follow-up comment measured that in the 5.1 templates they differ only in `end`, which comes from each block's half-length and the LFE's 250 Hz limit: for six channels at 44.1 and 48 kHz, qualities 0 to 1, the short block's setup gives 6 and the long block's 12 (at quality -0.1, 6 and 24). The header holds one residue 2 for both mappings, so one `end` serves both block sizes. The long block's, set second, keeps the LFE's lowpass near 250 Hz in long blocks and lets short blocks code to 12 coefficients, about 2.1 kHz; the other way round, long blocks would lose the LFE above 129 Hz. The comment offered two ways to remove the trap: keep the sharing deliberately (the larger `end`, the setup failing if anything else differs), or give the LFE a residue per block size, which changes 5.1 streams. He added a third, giving up shared submap numbers altogether, at a few header bytes for some coding efficiency, a trade-off nobody has measured. The choice is Xiph's, with Christopher Montgomery yet to answer; the encoded output of SymbolEngine's copy is the same as libvorbis' whichever way it goes, so it keeps freeing the first residue until libvorbis has a fix.
