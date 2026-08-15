# iso_data

Extracted contents of your own Jak game discs go here, one directory per game:

```
iso_data/jak1/    # Jak and Daxter: The Precursor Legacy
iso_data/jak2/    # Jak II
```

The extractor reads from these directories:

```sh
./build/decompiler/extractor --folder --decompile --game jak1 iso_data/jak1
```

**Nothing in here is ever committed.** The `.gitignore` files in this directory
and its subdirectories exclude all disc-derived data; only these placeholders are
tracked, so the layout is visible in a fresh clone.

This project ships no game assets. You need your own legally obtained copy of
each game — see the Legal section of the top-level [README](../README.md).
