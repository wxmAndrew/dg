#include <set>
#include <vector>
#include <string>
#include <cassert>
#include <iostream>
#include <fstream>

#ifndef HAVE_LLVM
#error "This code needs LLVM enabled"
#endif

#include <llvm/Config/llvm-config.h>

#if (LLVM_VERSION_MAJOR < 3)
#error "Unsupported version of LLVM"
#endif

#include "llvm-slicer.h"
#include "llvm-slicer-opts.h"
#include "llvm-slicer-utils.h"

// ignore unused parameters in LLVM libraries
#if (__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

#if LLVM_VERSION_MAJOR >= 4
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#else
#include <llvm/Bitcode/ReaderWriter.h>
#endif

//#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_os_ostream.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/PrettyStackTrace.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/IR/InstIterator.h>

#if (__clang__)
#pragma clang diagnostic pop // ignore -Wunused-parameter
#else
#pragma GCC diagnostic pop
#endif

#include "dg/llvm/SystemDependenceGraph/SDG2Dot.h"
#include "dg/llvm/SystemDependenceGraph/AnnotationWriter.h"
#include "dg/util/debug.h"

using namespace dg;

using llvm::errs;
using dg::analysis::LLVMPointerAnalysisOptions;
using dg::analysis::LLVMDataDependenceAnalysisOptions;

using AnnotationOptsT
    = dg::debug::LLVMDGAssemblyAnnotationWriter::AnnotationOptsT;

llvm::cl::opt<bool> enable_debug("dbg",
    llvm::cl::desc("Enable debugging messages (default=false)."),
    llvm::cl::init(false), llvm::cl::cat(SlicingOpts));

llvm::cl::opt<bool> statistics("statistics",
    llvm::cl::desc("Print statistics about slicing (default=false)."),
    llvm::cl::init(false), llvm::cl::cat(SlicingOpts));

llvm::cl::opt<bool> dump_bb_only("dump-bb-only",
    llvm::cl::desc("Only dump basic blocks of dependence graph to dot"
                   " (default=false)."),
    llvm::cl::init(false), llvm::cl::cat(SlicingOpts));

llvm::cl::opt<std::string> annotationOpts("annotate",
    llvm::cl::desc("Save annotated version of module as a text (.ll).\n"
                   "(dd: data dependencies, cd:control dependencies,\n"
                   "rd: reaching definitions, pta: points-to information,\n"
                   "slice: comment out what is going to be sliced away, etc.)\n"
                   "for more options, use comma separated list"),
    llvm::cl::value_desc("val1,val2,..."), llvm::cl::init(""),
    llvm::cl::cat(SlicingOpts));

class SDGDumper {
    const SlicerOptions& options;
    llvmdg::SystemDependenceGraph *dg;
    bool bb_only{false};
    uint32_t dump_opts{0};//{debug::PRINT_DD | debug::PRINT_CD | debug::PRINT_USE | debug::PRINT_ID};

public:
    SDGDumper(const SlicerOptions& opts,
              llvmdg::SystemDependenceGraph *dg,
              bool bb_only = false,
              uint32_t dump_opts = 0)
    : options(opts), dg(dg), bb_only(bb_only), dump_opts(dump_opts) {}

    void dumpToDot(const char *suffix = nullptr) {
        // compose new name
        std::string fl(options.inputFile);
        if (suffix)
            replace_suffix(fl, suffix);
        else
            replace_suffix(fl, ".dot");

        errs() << "Dumping SDG to " << fl << "\n";

        if (bb_only) {
            assert(false && "Not implemented");
            //SDGDumpBlocks dumper(dg, dump_opts, fl.c_str());
            //dumper.dump();
        } else {
            SDG2Dot dumper(dg, dump_opts, fl.c_str());
            dumper.dump();
        }
    }
};

class ModuleAnnotator {
    const SlicerOptions& options;
    llvmdg::SystemDependenceGraph *dg;
    AnnotationOptsT annotationOptions;

public:
    ModuleAnnotator(const SlicerOptions& o,
                    llvmdg::SystemDependenceGraph *dg,
                    AnnotationOptsT annotO)
    : options(o), dg(dg), annotationOptions(annotO) {}

    bool shouldAnnotate() const { return annotationOptions != 0; }

    void annotate(const std::set<LLVMNode *> *criteria = nullptr)
    {
        // compose name
        std::string fl(options.inputFile);
        fl.replace(fl.end() - 3, fl.end(), "-debug.ll");

        // open stream to write to
        std::ofstream ofs(fl);
        llvm::raw_os_ostream outputstream(ofs);

        std::string module_comment =
        "; -- Generated by llvm-slicer --\n"
        ";   * slicing criteria: '" + options.slicingCriteria + "'\n" +
        ";   * secondary slicing criteria: '" + options.secondarySlicingCriteria + "'\n" +
        ";   * forward slice: '" + std::to_string(options.forwardSlicing) + "'\n" +
        ";   * remove slicing criteria: '"
             + std::to_string(options.removeSlicingCriteria) + "'\n" +
        ";   * undefined are pure: '"
             + std::to_string(options.dgOptions.DDAOptions.undefinedArePure) + "'\n" +
        ";   * pointer analysis: ";
        if (options.dgOptions.PTAOptions.analysisType
                == LLVMPointerAnalysisOptions::AnalysisType::fi)
            module_comment += "flow-insensitive\n";
        else if (options.dgOptions.PTAOptions.analysisType
                    == LLVMPointerAnalysisOptions::AnalysisType::fs)
            module_comment += "flow-sensitive\n";
        else if (options.dgOptions.PTAOptions.analysisType
                    == LLVMPointerAnalysisOptions::AnalysisType::inv)
            module_comment += "flow-sensitive with invalidate\n";

        module_comment+= ";   * PTA field sensitivity: ";
        if (options.dgOptions.PTAOptions.fieldSensitivity == Offset::UNKNOWN)
            module_comment += "full\n\n";
        else
            module_comment
                += std::to_string(*options.dgOptions.PTAOptions.fieldSensitivity)
                   + "\n\n";

        errs() << "[llvm-slicer] Saving IR with annotations to " << fl << "\n";
        auto annot
            = new dg::debug::LLVMDGAssemblyAnnotationWriter(annotationOptions,
                                                            dg->getPTA(),
                                                            dg->getRDA(),
                                                            criteria);
        annot->emitModuleComment(std::move(module_comment));
        llvm::Module *M = dg->getModule();
        M->print(outputstream, annot);

        delete annot;
    }
};


static AnnotationOptsT parseAnnotationOptions(const std::string& annot)
{
    if (annot.empty())
        return {};

    AnnotationOptsT opts{};
    std::vector<std::string> lst = splitList(annot);
    for (const std::string& opt : lst) {
        if (opt == "dd")
            opts |= AnnotationOptsT::ANNOTATE_DD;
        else if (opt == "cd")
            opts |= AnnotationOptsT::ANNOTATE_CD;
        else if (opt == "rd")
            opts |= AnnotationOptsT::ANNOTATE_RD;
        else if (opt == "pta")
            opts |= AnnotationOptsT::ANNOTATE_PTR;
        else if (opt == "slice" || opt == "sl" || opt == "slicer")
            opts |= AnnotationOptsT::ANNOTATE_SLICE;
    }

    return opts;
}

std::unique_ptr<llvm::Module> parseModule(llvm::LLVMContext& context,
                                          const SlicerOptions& options)
{
    llvm::SMDiagnostic SMD;

#if ((LLVM_VERSION_MAJOR == 3) && (LLVM_VERSION_MINOR <= 5))
    auto _M = llvm::ParseIRFile(options.inputFile, SMD, context);
    auto M = std::unique_ptr<llvm::Module>(_M);
#else
    auto M = llvm::parseIRFile(options.inputFile, SMD, context);
    // _M is unique pointer, we need to get Module *
#endif

    if (!M) {
        SMD.print("llvm-slicer", llvm::errs());
    }

    return M;
}

#ifndef USING_SANITIZERS
void setupStackTraceOnError(int argc, char *argv[])
{

#if LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR < 9
    llvm::sys::PrintStackTraceOnErrorSignal();
#else
    llvm::sys::PrintStackTraceOnErrorSignal(llvm::StringRef());
#endif
    llvm::PrettyStackTraceProgram X(argc, argv);

}
#else
void setupStackTraceOnError(int, char **) {}
#endif // not USING_SANITIZERS

/*
// defined in llvm-slicer-crit.cpp
std::set<LLVMNode *> getSlicingCriteriaNodes(LLVMDependenceGraph& dg,
                                             const std::string& slicingCriteria);

std::pair<std::set<std::string>, std::set<std::string>>
parseSecondarySlicingCriteria(const std::string& slicingCriteria);

bool findSecondarySlicingCriteria(std::set<LLVMNode *>& criteria_nodes,
                                  const std::set<std::string>& secondaryControlCriteria,
                                  const std::set<std::string>& secondaryDataCriteria);
*/

int main(int argc, char *argv[])
{
    setupStackTraceOnError(argc, argv);

    SlicerOptions options = parseSlicerOptions(argc, argv);

    if (enable_debug)
        DBG_ENABLE();

    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> M = parseModule(context, options);
    if (!M) {
        llvm::errs() << "Failed parsing '" << options.inputFile << "' file:\n";
        return 1;
    }

    if (!M->getFunction(options.dgOptions.entryFunction)) {
        llvm::errs() << "The entry function not found: "
                     << options.dgOptions.entryFunction << "\n";
        return 1;
    }

    ModuleAnnotator annotator(options, &slicer.getDG(),
                              parseAnnotationOptions(annotationOpts));


    /*
    auto criteria_nodes = getSlicingCriteriaNodes(slicer.getDG(),
                                                  options.slicingCriteria);
    if (criteria_nodes.empty()) {
        llvm::errs() << "Did not find slicing criteria: '"
                     << options.slicingCriteria << "'\n";
        if (annotator.shouldAnnotate()) {
            slicer.computeDependencies();
        }
    }

    auto secondaryCriteria
        = parseSecondarySlicingCriteria(options.secondarySlicingCriteria);
    const auto& secondaryControlCriteria = secondaryCriteria.first;
    const auto& secondaryDataCriteria = secondaryCriteria.second;

    // mark nodes that are going to be in the slice
    if (!findSecondarySlicingCriteria(criteria_nodes,
                                      secondaryControlCriteria,
                                      secondaryDataCriteria)) {
        llvm::errs() << "Finding secondary slicing criteria nodes failed\n";
        return 1;
    }
    */

    // print debugging llvm IR if user asked for it
    if (annotator.shouldAnnotate())
        annotator.annotate(&criteria_nodes);

    SDGDumper dumper(options, &slicer.getDG(), dump_bb_only);
    dumper.dumpToDot();

    return 0;
}