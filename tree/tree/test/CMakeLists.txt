# Copyright (C) 1995-2019, Rene Brun and Fons Rademakers.
# All rights reserved.
#
# For the licensing terms see $ROOTSYS/LICENSE.
# For the list of contributors see $ROOTSYS/README/CREDITS.


ROOT_GENERATE_DICTIONARY(ElementStructDict ElementStruct.h LINKDEF ElementStructLinkDef.h OPTIONS -inlineInputHeader)
ROOT_ADD_GTEST(testTOffsetGeneration TOffsetGeneration.cxx ElementStruct.cxx ElementStructDict.cxx
  LIBRARIES RIO Tree MathCore
)
if(MSVC AND NOT CMAKE_GENERATOR MATCHES Ninja)
  add_custom_command(TARGET testTOffsetGeneration POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/libElementStructDict_rdict.pcm
                                     ${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/libElementStructDict_rdict.pcm)
endif()
target_include_directories(testTOffsetGeneration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
ROOT_STANDARD_LIBRARY_PACKAGE(SillyStruct NO_INSTALL_HEADERS HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/SillyStruct.h SOURCES SillyStruct.cxx LINKDEF SillyStructLinkDef.h DEPENDENCIES RIO)
ROOT_ADD_GTEST(testBulkApi BulkApi.cxx LIBRARIES RIO Tree TreePlayer)
#FIXME: tests are having timeout on 32bit CERN VM (in docker container everything is fine),
# to be reverted after investigation.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  ROOT_ADD_GTEST(testBulkApiMultiple BulkApiMultiple.cxx LIBRARIES RIO Tree TreePlayer TIMEOUT 3000)
  ROOT_ADD_GTEST(testBulkApiVarLength BulkApiVarLength.cxx LIBRARIES RIO Tree TreePlayer)
  ROOT_ADD_GTEST(testBulkApiSillyStruct BulkApiSillyStruct.cxx LIBRARIES RIO Tree TreePlayer SillyStruct)
endif()
ROOT_ADD_GTEST(testTBasket TBasket.cxx LIBRARIES RIO Tree)
ROOT_ADD_GTEST(testTBranch TBranch.cxx LIBRARIES RIO Tree MathCore)
ROOT_ADD_GTEST(testTIOFeatures TIOFeatures.cxx LIBRARIES RIO Tree)
ROOT_ADD_GTEST(testTTreeCluster TTreeClusterTest.cxx LIBRARIES RIO Tree MathCore)
ROOT_ADD_GTEST(testTChainParsing TChainParsing.cxx LIBRARIES RIO Tree)
if(imt)
   ROOT_ADD_GTEST(testTTreeImplicitMT ImplicitMT.cxx LIBRARIES RIO Tree)
endif()
ROOT_ADD_GTEST(testTChainSaveAsCxx TChainSaveAsCxx.cxx LIBRARIES RIO Tree)
ROOT_ADD_GTEST(testTChainRegressions TChainRegressions.cxx LIBRARIES RIO Tree)
ROOT_ADD_GTEST(testTTreeTruncatedDatatypes TTreeTruncatedDatatypes.cxx LIBRARIES RIO Tree)
ROOT_ADD_GTEST(testTTreeRegressions TTreeRegressions.cxx LIBRARIES RIO Tree Hist)
ROOT_ADD_GTEST(entrylist_addsublist entrylist_addsublist.cxx LIBRARIES RIO Tree)
ROOT_ADD_GTEST(chain_setentrylist chain_setentrylist.cxx LIBRARIES RIO Tree)
ROOT_ADD_GTEST(entrylist_enter entrylist_enter.cxx LIBRARIES RIO Tree)
ROOT_ADD_GTEST(entrylist_enterrange entrylist_enterrange.cxx LIBRARIES RIO Tree)
ROOT_ADD_GTEST(friendinfo friendinfo.cxx LIBRARIES RIO Tree)
ROOT_ADD_GTEST(testTTreeCompression TTreeCompression.cxx LIBRARIES RIO Tree MathCore)
