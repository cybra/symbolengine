# opusfile: opening a chained file can lose a link's tags

Where it goes: a new issue on [Xiph's GitLab](https://gitlab.xiph.org/xiph/opusfile/-/issues). A search of the tracker for "leak" finds nothing on this; the code is unchanged on `master`.

## Report

**op_open_callbacks() leaks a link's tags when that link has no usable audio**

`op_bisect_forward_serialno()` fetches the headers of each further link in a chained file, which fills `links[nlinks].tags`, and then calls `op_find_initial_pcm_offset()`. If that fails, the function returns at once, before `_of->nlinks` is raised:

```c
ret=op_fetch_headers(_of,&links[nlinks].head,&links[nlinks].tags,
 _serialnos,_nserialnos,_cserialnos,last!=next?NULL:&og);
if(OP_UNLIKELY(ret<0))return ret;
...
ret=op_find_initial_pcm_offset(_of,links+nlinks,NULL);
if(OP_UNLIKELY(ret<0))return ret;
links[nlinks].pcm_file_offset=total_duration;
_searched=_of->offset;
/*Mark the current link count so it can be cleaned up on error.*/
_of->nlinks=++nlinks;
```

`op_clear()` frees the tags of links below `_of->nlinks`, so this link's are never freed. A damaged or crafted chained file loses a little memory on every open. libFuzzer with AddressSanitizer found it, reading a file through `op_open_callbacks()`:

```
Direct leak of 4 byte(s) in 1 object(s) allocated from:
    #0 realloc
    #1 op_tags_ensure_capacity src/info.c:122
    #2 opus_tags_parse_impl src/info.c:193
    #3 opus_tags_parse src/info.c:235
    #4 op_fetch_headers_impl src/opusfile.c:549
    #5 op_fetch_headers src/opusfile.c:584
    #6 op_bisect_forward_serialno src/opusfile.c:1267
    #7 op_open_seekable2_impl src/opusfile.c:1413
```

The fix:

```diff
--- a/src/opusfile.c
+++ b/src/opusfile.c
@@ op_bisect_forward_serialno
     ret=op_find_initial_pcm_offset(_of,links+nlinks,NULL);
-    if(OP_UNLIKELY(ret<0))return ret;
+    if(OP_UNLIKELY(ret<0)){
+      opus_tags_clear(&links[nlinks].tags);
+      return ret;
+    }
     links[nlinks].pcm_file_offset=total_duration;
```

Measured on the file that found it, 35,379 bytes, with macOS's `leaks --atExit`: 3 leaks of 80 bytes before the change, and none after. I can attach the file.
