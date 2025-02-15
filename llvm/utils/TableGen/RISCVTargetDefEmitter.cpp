//===- RISCVTargetDefEmitter.cpp - Generate lists of RISC-V CPUs ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This tablegen backend emits the include file needed by RISCVTargetParser.cpp
// and RISCVISAInfo.cpp to parse the RISC-V CPUs and extensions.
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/RISCVISAUtils.h"
#include "llvm/TableGen/Record.h"
#include "llvm/TableGen/TableGenBackend.h"

using namespace llvm;

static StringRef getExtensionName(const Record *R) {
  StringRef Name = R->getValueAsString("Name");
  Name.consume_front("experimental-");
  return Name;
}

static void printExtensionTable(raw_ostream &OS,
                                const std::vector<Record *> &Extensions,
                                bool Experimental) {
  OS << "static const RISCVSupportedExtension Supported";
  if (Experimental)
    OS << "Experimental";
  OS << "Extensions[] = {\n";

  for (Record *R : Extensions) {
    if (R->getValueAsBit("Experimental") != Experimental)
      continue;

    OS << "    {\"" << getExtensionName(R) << "\", {"
       << R->getValueAsInt("MajorVersion") << ", "
       << R->getValueAsInt("MinorVersion") << "}},\n";
  }

  OS << "};\n\n";
}

static void emitRISCVExtensions(RecordKeeper &Records, raw_ostream &OS) {
  OS << "#ifdef GET_SUPPORTED_EXTENSIONS\n";
  OS << "#undef GET_SUPPORTED_EXTENSIONS\n\n";

  std::vector<Record *> Extensions =
      Records.getAllDerivedDefinitions("RISCVExtension");
  llvm::sort(Extensions, [](const Record *Rec1, const Record *Rec2) {
    return getExtensionName(Rec1) < getExtensionName(Rec2);
  });

  printExtensionTable(OS, Extensions, /*Experimental=*/false);
  printExtensionTable(OS, Extensions, /*Experimental=*/true);

  OS << "#endif // GET_SUPPORTED_EXTENSIONS\n\n";

  OS << "#ifdef GET_IMPLIED_EXTENSIONS\n";
  OS << "#undef GET_IMPLIED_EXTENSIONS\n\n";

  OS << "\nstatic constexpr ImpliedExtsEntry ImpliedExts[] = {\n";
  for (Record *Ext : Extensions) {
    auto ImpliesList = Ext->getValueAsListOfDefs("Implies");
    if (ImpliesList.empty())
      continue;

    StringRef Name = getExtensionName(Ext);

    for (auto *ImpliedExt : ImpliesList) {
      if (!ImpliedExt->isSubClassOf("RISCVExtension"))
        continue;

      OS << "    { {\"" << Name << "\"}, \"" << getExtensionName(ImpliedExt)
         << "\"},\n";
    }
  }

  OS << "};\n\n";

  OS << "#endif // GET_IMPLIED_EXTENSIONS\n\n";
}

// We can generate march string from target features as what has been described
// in RISC-V ISA specification (version 20191213) 'Chapter 27. ISA Extension
// Naming Conventions'.
//
// This is almost the same as RISCVFeatures::parseFeatureBits, except that we
// get feature name from feature records instead of feature bits.
static void printMArch(raw_ostream &OS, const Record &Rec) {
  std::map<std::string, RISCVISAUtils::ExtensionVersion,
           RISCVISAUtils::ExtensionComparator>
      Extensions;
  unsigned XLen = 0;

  // Convert features to FeatureVector.
  for (auto *Feature : Rec.getValueAsListOfDefs("Features")) {
    StringRef FeatureName = Feature->getValueAsString("Name");
    if (Feature->isSubClassOf("RISCVExtension")) {
      unsigned Major = Feature->getValueAsInt("MajorVersion");
      unsigned Minor = Feature->getValueAsInt("MinorVersion");
      Extensions[FeatureName.str()] = {Major, Minor};
    } else if (FeatureName == "64bit") {
      assert(XLen == 0 && "Already determined XLen");
      XLen = 64;
    } else if (FeatureName == "32bit") {
      assert(XLen == 0 && "Already determined XLen");
      XLen = 32;
    }
  }

  assert(XLen != 0 && "Unable to determine XLen");

  OS << "rv" << XLen;

  ListSeparator LS("_");
  for (auto const &Ext : Extensions)
    OS << LS << Ext.first << Ext.second.Major << 'p' << Ext.second.Minor;
}

static void emitRISCVProcs(RecordKeeper &RK, raw_ostream &OS) {
  OS << "#ifndef PROC\n"
     << "#define PROC(ENUM, NAME, DEFAULT_MARCH, FAST_UNALIGNED_ACCESS)\n"
     << "#endif\n\n";

  // Iterate on all definition records.
  for (const Record *Rec : RK.getAllDerivedDefinitions("RISCVProcessorModel")) {
    bool FastScalarUnalignedAccess =
        any_of(Rec->getValueAsListOfDefs("Features"), [&](auto &Feature) {
          return Feature->getValueAsString("Name") == "unaligned-scalar-mem";
        });

    bool FastVectorUnalignedAccess =
        any_of(Rec->getValueAsListOfDefs("Features"), [&](auto &Feature) {
          return Feature->getValueAsString("Name") == "unaligned-vector-mem";
        });

    bool FastUnalignedAccess =
        FastScalarUnalignedAccess && FastVectorUnalignedAccess;

    OS << "PROC(" << Rec->getName() << ", {\"" << Rec->getValueAsString("Name")
       << "\"}, {\"";

    StringRef MArch = Rec->getValueAsString("DefaultMarch");

    // Compute MArch from features if we don't specify it.
    if (MArch.empty())
      printMArch(OS, *Rec);
    else
      OS << MArch;
    OS << "\"}, " << FastUnalignedAccess << ")\n";
  }
  OS << "\n#undef PROC\n";
  OS << "\n";
  OS << "#ifndef TUNE_PROC\n"
     << "#define TUNE_PROC(ENUM, NAME)\n"
     << "#endif\n\n";

  for (const Record *Rec :
       RK.getAllDerivedDefinitions("RISCVTuneProcessorModel")) {
    OS << "TUNE_PROC(" << Rec->getName() << ", "
       << "\"" << Rec->getValueAsString("Name") << "\")\n";
  }

  OS << "\n#undef TUNE_PROC\n";
}

static void EmitRISCVTargetDef(RecordKeeper &RK, raw_ostream &OS) {
  emitRISCVExtensions(RK, OS);
  emitRISCVProcs(RK, OS);
}

static TableGen::Emitter::Opt X("gen-riscv-target-def", EmitRISCVTargetDef,
                                "Generate the list of CPUs and extensions for "
                                "RISC-V");
