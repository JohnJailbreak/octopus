# Reproducing the analysis

Tools used: Python 3.10 inventory parser; Ghidra 12.1.4 headless with JDK21; ipsw utility for supplementary Objective-C output. Ghidra performed ARM64e decoding, chained-fixup analysis, Objective-C recovery, cross-references and decompilation. REA MCP was unavailable; these are direct tool outputs, not REA Evidence records.

Preserve the five reference files outside the repository and verify their hashes against binary-inventory.md.

```text
python docs/usb-research/tools/macho_inventory.py /path/to/reference /path/to/inventory
analyzeHeadless /path/to/projects Octopus -import /path/to/reference/ptpd -scriptPath docs/usb-research/tools -postScript ExportEvidence.java /path/to/evidence ".*"
```

Repeat import/export for the other binaries, or use ExportNamedEvidence.java on a project containing all five. On Windows use support/analyzeHeadless.bat. Keep Ghidra config/cache/output in writable workspace directories. Scripts never execute target code. Inventory parser is an analyst utility for these known files, not a hardened untrusted-input parser; the application's identity parser is independently bounded and tested.

Evidence addresses are unslid VAs. Function indexes may include Ghidra synthetic EXTERNAL entries; these are not executable target code. Selected evidence excludes those imported placeholders. Method encodings were recovered from Objective-C relative method metadata and cross-checked against implementation addresses; do not use them as evidence of runtime hook compatibility. Pseudocode has decompiler limitations, including inferred signatures and PAC artifacts. Consult adjoining disassembly.

Retained text exports have trailing whitespace and redundant final blank lines normalized; instruction and pseudocode content is unchanged.
