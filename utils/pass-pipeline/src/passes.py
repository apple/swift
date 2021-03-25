
from pass_pipeline import Pass

# TODO: This should not be hard coded. Create a tool in the compiler that knows
# how to dump the passes and the pipelines themselves.
AADumper = Pass('AADumper')
ABCOpt = Pass('ABCOpt')
AddressLowering = Pass('AddressLowering')
AllocBoxToStack = Pass('AllocBoxToStack')
CFGPrinter = Pass('CFGPrinter')
COWArrayOpts = Pass('COWArrayOpts')
CSE = Pass('CSE')
CapturePromotion = Pass('CapturePromotion')
CapturePropagation = Pass('CapturePropagation')
ClosureSpecializer = Pass('ClosureSpecializer')
CodeMotion = Pass('CodeMotion')
CopyForwarding = Pass('CopyForwarding')
DCE = Pass('DCE')
DeadFunctionElimination = Pass('DeadFunctionElimination')
DeadObjectElimination = Pass('DeadObjectElimination')
DefiniteInitialization = Pass('DefiniteInitialization')
DiagnoseVarUsage = Pass('DiagnoseVarUsage')
DiagnoseUnreachable = Pass('DiagnoseUnreachable')
DiagnosticConstantPropagation = Pass('DiagnosticConstantPropagation')
EarlyInliner = Pass('EarlyInliner')
EmitDFDiagnostics = Pass('EmitDFDiagnostics')
FunctionSignatureOpts = Pass('FunctionSignatureOpts')
GlobalARCOpts = Pass('GlobalARCOpts')
GlobalLoadStoreOpts = Pass('GlobalLoadStoreOpts')
GlobalOpt = Pass('GlobalOpt')
IVInfoPrinter = Pass('IVInfoPrinter')
InstCount = Pass('InstCount')
LICM = Pass('LICM')
LateInliner = Pass('LateInliner')
LoopInfoPrinter = Pass('LoopInfoPrinter')
LoopRotate = Pass('LoopRotate')
LowerAggregateInstrs = Pass('LowerAggregateInstrs')
MandatoryInlining = Pass('MandatoryInlining')
Mem2Reg = Pass('Mem2Reg')
NoReturnFolding = Pass('NoReturnFolding')
PerfInliner = Pass('PerfInliner')
PerformanceConstantPropagation = Pass('PerformanceConstantPropagation')
PredictableMemoryOptimizations = Pass('PredictableMemoryOptimizations')
IRGenPrepare = Pass('IRGenPrepare')
SILCombine = Pass('SILCombine')
SILLinker = Pass('SILLinker')
SROA = Pass('SROA')
SimplifyCFG = Pass('SimplifyCFG')
SpeculativeDevirtualizer = Pass('SpeculativeDevirtualizer')
SplitAllCriticalEdges = Pass('SplitAllCriticalEdges')
SplitNonCondBrCriticalEdges = Pass('SplitNonCondBrCriticalEdges')
StripDebugInfo = Pass('StripDebugInfo')
SwiftArrayOpts = Pass('SwiftArrayOpts')

PASSES = [
    AADumper,
    ABCOpt,
    AddressLowering,
    AllocBoxToStack,
    CFGPrinter,
    COWArrayOpts,
    CSE,
    CapturePromotion,
    CapturePropagation,
    ClosureSpecializer,
    CodeMotion,
    CopyForwarding,
    DCE,
    DeadFunctionElimination,
    DeadObjectElimination,
    DefiniteInitialization,
    DiagnoseVarUsage,
    DiagnoseUnreachable,
    DiagnosticConstantPropagation,
    EarlyInliner,
    EmitDFDiagnostics,
    FunctionSignatureOpts,
    GlobalARCOpts,
    GlobalLoadStoreOpts,
    GlobalOpt,
    IVInfoPrinter,
    InstCount,
    LICM,
    LateInliner,
    LoopInfoPrinter,
    LoopRotate,
    LowerAggregateInstrs,
    MandatoryInlining,
    Mem2Reg,
    NoReturnFolding,
    PerfInliner,
    PerformanceConstantPropagation,
    PredictableMemoryOptimizations,
    IRGenPrepare,
    SILCombine,
    SILLinker,
    SROA,
    SimplifyCFG,
    SpeculativeDevirtualizer,
    SplitAllCriticalEdges,
    SplitNonCondBrCriticalEdges,
    StripDebugInfo,
    SwiftArrayOpts,
]
