# Copyright (C) 1995-2019, Rene Brun and Fons Rademakers.
# All rights reserved.
#
# For the licensing terms see $ROOTSYS/LICENSE.
# For the list of contributors see $ROOTSYS/README/CREDITS.

add_subdirectory(math)

set(MINUIT2_HEADERS
    AnalyticalGradientCalculator.h
    BFGSErrorUpdator.h
    CombinedMinimizer.h
    CombinedMinimumBuilder.h
    ContoursError.h
    DavidonErrorUpdator.h
    ExternalInternalGradientCalculator.h
    FCNAdapter.h
    FCNBase.h
    FCNGradAdapter.h
    FCNGradientBase.h
    FumiliBuilder.h
    FumiliChi2FCN.h
    FumiliErrorUpdator.h
    FumiliFCNAdapter.h
    FumiliFCNBase.h
    FumiliGradientCalculator.h
    FumiliMaximumLikelihoodFCN.h
    FumiliMinimizer.h
    FumiliStandardChi2FCN.h
    FumiliStandardMaximumLikelihoodFCN.h
    FunctionGradient.h
    FunctionMinimum.h
    GradientCalculator.h
    HessianGradientCalculator.h
    InitialGradientCalculator.h
    MinimumBuilder.h
    MinimumError.h
    MinimumErrorUpdator.h
    MinimumParameters.h
    MinimumSeed.h
    MinimumSeedGenerator.h
    MinimumState.h
    MinosError.h
    Minuit2Minimizer.h
    MinuitParameter.h
    MnApplication.h
    MnConfig.h
    MnContours.h
    MnCovarianceSqueeze.h
    MnCross.h
    MnEigen.h
    MnFcn.h
    MnFumiliMinimize.h
    MnFunctionCross.h
    MnGlobalCorrelationCoeff.h
    MnHesse.h
    MnLineSearch.h
    MnMachinePrecision.h
    MnMatrix.h
    MnMatrixfwd.h
    MnMigrad.h
    MnMinimize.h
    MnMinos.h
    MnParabola.h
    MnParabolaFactory.h
    MnParameterScan.h
    MnParameterTransformation.h
    MnPlot.h
    MnPoint.h
    MnPosDef.h
    MnPrint.h
    MnScan.h
    MnSeedGenerator.h
    MnSimplex.h
    MnStrategy.h
    MnTraceObject.h
    MnUserCovariance.h
    MnUserParameterState.h
    MnUserParameters.h
    MnUserTransformation.h
    ModularFunctionMinimizer.h
    NegativeG2LineSearch.h
    Numerical2PGradientCalculator.h
    ParametricFunction.h
    ScanBuilder.h
    ScanMinimizer.h
    SimplexBuilder.h
    SimplexMinimizer.h
    SimplexParameters.h
    SimplexSeedGenerator.h
    VariableMetricBuilder.h
    VariableMetricEDMEstimator.h
    VariableMetricMinimizer.h
    )

set(MINUIT2_SOURCES
    AnalyticalGradientCalculator.cxx
    BFGSErrorUpdator.cxx
    CombinedMinimumBuilder.cxx
    DavidonErrorUpdator.cxx
    ExternalInternalGradientCalculator.cxx
    FumiliBuilder.cxx
    FumiliErrorUpdator.cxx
    FumiliGradientCalculator.cxx
    FumiliMinimizer.cxx
    FumiliStandardChi2FCN.cxx
    FumiliStandardMaximumLikelihoodFCN.cxx
    HessianGradientCalculator.cxx
    InitialGradientCalculator.cxx
    MPIProcess.cxx
    MinimumBuilder.cxx
    Minuit2Minimizer.cxx
    MnApplication.cxx
    MnContours.cxx
    MnCovarianceSqueeze.cxx
    MnEigen.cxx
    MnFcn.cxx
    MnFumiliMinimize.cxx
    MnFunctionCross.cxx
    MnGlobalCorrelationCoeff.cxx
    MnHesse.cxx
    MnLineSearch.cxx
    MnMachinePrecision.cxx
    MnMatrix.cxx
    MnMinos.cxx
    MnParabolaFactory.cxx
    MnParameterScan.cxx
    MnParameterTransformation.cxx
    MnPlot.cxx
    MnPosDef.cxx
    MnPrint.cxx
    MnPrintImpl.cxx
    MnScan.cxx
    MnSeedGenerator.cxx
    MnStrategy.cxx
    MnTraceObject.cxx
    MnUserParameterState.cxx
    MnUserParameters.cxx
    MnUserTransformation.cxx
    ModularFunctionMinimizer.cxx
    NegativeG2LineSearch.cxx
    Numerical2PGradientCalculator.cxx
    ParametricFunction.cxx
    ScanBuilder.cxx
    SimplexBuilder.cxx
    SimplexParameters.cxx
    SimplexSeedGenerator.cxx
    VariableMetricBuilder.cxx
    VariableMetricEDMEstimator.cxx
    mnbins.cxx
    mntplot.cxx
    )

prepend_path(MINUIT2_HEADERS "${Minuit2_SOURCE_DIR}/inc/Minuit2" ${MINUIT2_HEADERS})
prepend_path(MINUIT2_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}" ${MINUIT2_SOURCES})

add_library(Minuit2
    ${MINUIT2_SOURCES}
    ${MINUIT2_HEADERS}
    )

# Add alias for direct inclusion with add_subdirectory
add_library(Minuit2::Minuit2 ALIAS Minuit2)

target_include_directories(
    Minuit2
    PUBLIC
    $<BUILD_INTERFACE:${Minuit2_SOURCE_DIR}/inc>
    $<INSTALL_INTERFACE:include/Minuit2>
    )

target_compile_features(Minuit2 PUBLIC cxx_nullptr cxx_nonstatic_member_init)
set_target_properties(Minuit2 PROPERTIES CXX_EXTENSIONS OFF)

target_link_libraries(Minuit2 PUBLIC Minuit2Math Minuit2Common)

install(TARGETS Minuit2
        EXPORT Minuit2Targets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        )

install(FILES ${MINUIT2_HEADERS} DESTINATION include/Minuit2/Minuit2)
