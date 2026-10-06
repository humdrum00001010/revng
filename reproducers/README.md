# rev.ng issue reproducers

These are independently authored C programs and complete binaries for issues
[#645](https://github.com/revng/revng/issues/645) through
[#655](https://github.com/revng/revng/issues/655). The principal test command is:

```sh
revng quick artifact emit-recompilable-archive BINARY -o recompilable.tar.gz
```

Each directory contains its source, build and run instructions, pinned tool
identity, observed output, hashes, and the exact evidence boundary. No private
application binary, source, model, IR, function fragment, or identifier is
included.

| Issue | Result from the included complete binary |
|---|---|
| 645 | Reproduced: generated C uses incomplete array element types and does not compile. |
| 646 | Intended design: generated object retains `_undef_value`; the issue is closed. |
| 647 | Not reproduced by the two included DLL candidates. |
| 648 | Not reproduced by the included raw and DWARF ELF candidates. |
| 649 | Reproduced: executable PE export roots are absent from the generated model/source. |
| 650 | Not reproduced by the included ELF candidate. |
| 651 | Not reproduced by the two included DLL candidates. |
| 652 | Not reproduced; the compiler folds away the intended lookup in this candidate. |
| 653 | The post-inline GEP verifier failure is reproduced; the stale-policy cause remains unproven. |
| 654 | Reproduced with the included C address-layout control; the DLL alone is not claimed to fail. |
| 655 | Not reproduced by the included DLL candidate. |

Negative results apply only to the included candidates. Direct LLVM/Clift
fixtures are not treated as proof that the normal binary pipeline reaches a
reported defect.
