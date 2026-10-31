# libvorbis: opening a chained file can lose what a link read

Where it goes: a new issue on [Xiph's GitLab](https://gitlab.xiph.org/xiph/vorbis/-/issues), where libvorbis is maintained. A search of the tracker for "vorbisfile" and "serialno" finds nothing on this; the code is unchanged on `main`.

## Report

**ov_open_callbacks() leaks when a later link of a chained file fails to open**

`_bisect_forward_serialno()` in `lib/vorbisfile.c` keeps a list of each further link's serial numbers, and its `vorbis_info` and `vorbis_comment`, on its stack while it recurses, and returns at once if anything goes wrong:

```c
ret=_fetch_headers(vf,&vi,&vc,&next_serialno_list,&next_serialnos,NULL);
if(ret)return(ret);
...
ret=_bisect_forward_serialno(vf,next,vf->offset,end,endgran,endserial,
                             next_serialno_list,next_serialnos,m+1);
if(ret)return(ret);

if(next_serialno_list)_ogg_free(next_serialno_list);
```

`_fetch_headers()` adds to the list before it can fail, and clears only `vi` and `vc` as it does, so the first return loses the list. The second loses the list and `vi` and `vc`, which aren't yet in `vf->vi` and `vf->vc` for `ov_clear()` to find. A damaged or crafted chained file loses memory on every open. libFuzzer with AddressSanitizer found it:

```
Direct leak of 8 byte(s) in 1 object(s) allocated from:
    #0 malloc
    #1 _add_serialno lib/vorbisfile.c:195
    #2 _fetch_headers lib/vorbisfile.c:317
    #3 _bisect_forward_serialno
    #4 _open_seekable2 lib/vorbisfile.c:647
    #5 _ov_open2 lib/vorbisfile.c:943
    #6 ov_open_callbacks
```

The fix:

```diff
--- a/lib/vorbisfile.c
+++ b/lib/vorbisfile.c
@@ _bisect_forward_serialno
     ret=_fetch_headers(vf,&vi,&vc,&next_serialno_list,&next_serialnos,NULL);
-    if(ret)return(ret);
+    if(ret){
+      if(next_serialno_list)_ogg_free(next_serialno_list);
+      return(ret);
+    }
@@
     ret=_bisect_forward_serialno(vf,next,vf->offset,end,endgran,endserial,
                                  next_serialno_list,next_serialnos,m+1);
-    if(ret)return(ret);
+    if(ret){
+      if(next_serialno_list)_ogg_free(next_serialno_list);
+      vorbis_info_clear(&vi);
+      vorbis_comment_clear(&vc);
+      return(ret);
+    }
```

Measured on the file that found it, 11,168 bytes, with macOS's `leaks --atExit`: a leak from `_add_serialno` before the change, and none after. I can attach the file.
