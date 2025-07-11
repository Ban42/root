# Copyright (C) 1995-2022, Rene Brun and Fons Rademakers.
# All rights reserved.
#
# For the licensing terms see $ROOTSYS/LICENSE.
# For the list of contributors see $ROOTSYS/README/CREDITS.

############################################################################
# CMakeLists.txt file for building ROOT tree/dataframe package
# @author Danilo Piparo CERN, Pere Mato CERN
############################################################################

if(NOT dataframe)
    return()
endif()

if(arrow)
  list(APPEND RDATAFRAME_EXTRA_HEADERS ROOT/RArrowDS.hxx)
  list(APPEND RDATAFRAME_EXTRA_INCLUDES -I${ARROW_INCLUDE_DIR})
endif()

if(sqlite)
  list(APPEND RDATAFRAME_EXTRA_HEADERS ROOT/RSqliteDS.hxx)
endif()

if (imt)
  list(APPEND RDATAFRAME_EXTRA_DEPS Imt)
endif(imt)

set (EXTRA_DICT_OPTS)
if (runtime_cxxmodules AND WIN32)
  set (EXTRA_DICT_OPTS NO_CXXMODULE)
endif()

ROOT_STANDARD_LIBRARY_PACKAGE(ROOTDataFrame
  HEADERS
    ROOT/RCsvDS.hxx
    ROOT/RVecDS.hxx
    ROOT/RDataFrame.hxx
    ROOT/RDataSource.hxx
    ROOT/RDFHelpers.hxx
    ROOT/RLazyDS.hxx
    ROOT/RResultHandle.hxx
    ROOT/RResultPtr.hxx
    ROOT/RRootDS.hxx
    ROOT/RSnapshotOptions.hxx
    ROOT/RTrivialDS.hxx
    ROOT/RTTreeDS.hxx
    ROOT/RDF/ActionHelpers.hxx
    ROOT/RDF/ColumnReaderUtils.hxx
    ROOT/RDF/GraphNode.hxx
    ROOT/RDF/GraphUtils.hxx
    ROOT/RDF/HistoModels.hxx
    ROOT/RDF/InterfaceUtils.hxx
    ROOT/RDF/RActionBase.hxx
    ROOT/RDF/RAction.hxx
    ROOT/RDF/RActionSnapshot.hxx
    ROOT/RDF/RActionImpl.hxx
    ROOT/RDF/RColumnRegister.hxx
    ROOT/RDF/RNewSampleNotifier.hxx
    ROOT/RDF/RSampleInfo.hxx
    ROOT/RDF/RDefineBase.hxx
    ROOT/RDF/RDefaultValueFor.hxx
    ROOT/RDF/RDefine.hxx
    ROOT/RDF/RDefinePerSample.hxx
    ROOT/RDF/RDefineReader.hxx
    ROOT/RDF/RDSColumnReader.hxx
    ROOT/RDF/RColumnReaderBase.hxx
    ROOT/RDF/RCutFlowReport.hxx
    ROOT/RDF/RDatasetSpec.hxx
    ROOT/RDF/RDisplay.hxx
    ROOT/RDF/RFilterBase.hxx
    ROOT/RDF/RFilter.hxx
    ROOT/RDF/RInterface.hxx
    ROOT/RDF/RInterfaceBase.hxx
    ROOT/RDF/RJittedAction.hxx
    ROOT/RDF/RJittedDefine.hxx
    ROOT/RDF/RJittedFilter.hxx
    ROOT/RDF/RJittedVariation.hxx
    ROOT/RDF/RLazyDSImpl.hxx
    ROOT/RDF/RLoopManager.hxx
    ROOT/RDF/RMergeableValue.hxx
    ROOT/RDF/RMetaData.hxx
    ROOT/RDF/RNodeBase.hxx
    ROOT/RDF/RRangeBase.hxx
    ROOT/RDF/RRange.hxx
    ROOT/RDF/RResultMap.hxx
    ROOT/RDF/RSample.hxx
    ROOT/RDF/RFilterWithMissingValues.hxx
    ROOT/RDF/RTreeColumnReader.hxx
    ROOT/RDF/RVariation.hxx
    ROOT/RDF/RVariationBase.hxx
    ROOT/RDF/RVariationReader.hxx
    ROOT/RDF/RVariationsDescription.hxx
    ROOT/RDF/RVariedAction.hxx
    ROOT/RDF/SnapshotHelpers.hxx
    ROOT/RDF/Utils.hxx
    ROOT/RDF/PyROOTHelpers.hxx
    ROOT/RDF/RDFDescription.hxx
    ROOT/RNTupleDS.hxx
    ${RDATAFRAME_EXTRA_HEADERS}
  SOURCES
    src/RActionBase.cxx
    src/RCsvDS.cxx
    src/RDefineBase.cxx
    src/RDefineReader.cxx
    src/RCutFlowReport.cxx
    src/RDataFrame.cxx
    src/RDatasetSpec.cxx
    src/RDataSource.cxx
    src/RDFActionHelpers.cxx
    src/RDFColumnReaderUtils.cxx
    src/RDFColumnRegister.cxx
    src/RDFColumnReaderUtils.cxx
    src/RDFDisplay.cxx
    src/RDFGraphUtils.cxx
    src/RDFHistoModels.cxx
    src/RDFInterfaceUtils.cxx
    src/RDFSnapshotHelpers.cxx
    src/RDFUtils.cxx
    src/RDFHelpers.cxx
    src/RFilterBase.cxx
    src/RInterfaceBase.cxx
    src/RInterface.cxx
    src/RJittedAction.cxx
    src/RJittedDefine.cxx
    src/RJittedFilter.cxx
    src/RJittedVariation.cxx
    src/RLoopManager.cxx
    src/RMetaData.cxx
    src/RRangeBase.cxx
    src/RSample.cxx
    src/RTreeColumnReader.cxx
    src/RResultPtr.cxx
    src/RTTreeDS.cxx
    src/RVariationBase.cxx
    src/RVariationReader.cxx
    src/RVariationsDescription.cxx
    src/RRootDS.cxx
    src/RTrivialDS.cxx
    src/RDFDescription.cxx
    src/RNTupleDS.cxx
  DICTIONARY_OPTIONS
    -writeEmptyRootPCM
    ${RDATAFRAME_EXTRA_INCLUDES}
  DEPENDENCIES
    Tree
    TreePlayer
    Hist
    RIO
    ROOTVecOps
    ROOTNTuple
    ${RDATAFRAME_EXTRA_DEPS}
  ${EXTRA_DICT_OPTS}
)

if(arrow)
  target_sources(ROOTDataFrame PRIVATE src/RArrowDS.cxx)
  target_include_directories(ROOTDataFrame PRIVATE ${ARROW_INCLUDE_DIR})
  target_link_libraries(ROOTDataFrame PRIVATE ${ARROW_SHARED_LIB})
endif()

if(sqlite)
  target_sources(ROOTDataFrame PRIVATE src/RSqliteDS.cxx)
  target_include_directories(ROOTDataFrame PRIVATE ${SQLITE_INCLUDE_DIR})
  target_link_libraries(ROOTDataFrame PRIVATE ${SQLITE_LIBRARIES})
endif()

if(MSVC)
  target_compile_definitions(ROOTDataFrame PRIVATE _USE_MATH_DEFINES)
endif()

target_link_libraries(ROOTDataFrame PRIVATE nlohmann_json::nlohmann_json)

ROOT_ADD_TEST_SUBDIRECTORY(test)
