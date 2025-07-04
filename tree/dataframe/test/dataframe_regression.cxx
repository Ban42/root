#include "ROOT/RDataFrame.hxx"
#include "TBranchObject.h"
#include "TBranchElement.h"
#include "TROOT.h"
#include "TVector3.h"
#include "TSystem.h"
#include "Math/Vector4D.h"
#include "ROOT/TestSupport.hxx"

#include <algorithm>

#include "gtest/gtest.h"

#include <TFile.h>

// Fixture for all tests in this file. If parameter is true, run with implicit MT, else run sequentially
class RDFRegressionTests : public ::testing::TestWithParam<bool> {
protected:
   RDFRegressionTests() : NSLOTS(GetParam() ? 4u : 1u)
   {
      if (GetParam())
         ROOT::EnableImplicitMT(NSLOTS);
   }
   ~RDFRegressionTests() override
   {
      if (GetParam())
         ROOT::DisableImplicitMT();
   }
   const unsigned int NSLOTS;
};

void FillTree(const char *filename, const char *treeName, int nevents = 0)
{
   TFile f(filename, "RECREATE");
   TTree t(treeName, treeName);
   t.SetAutoFlush(1); // yes, one event per cluster: to make MT more meaningful
   int b;
   t.Branch("b1", &b);
   for (int i = 0; i < nevents; ++i) {
      b = i;
      t.Fill();
   }
   t.Write();
   f.Close();
}

// https://github.com/root-project/root/issues/11207
TEST(RDFRegressionTests, AliasAndSubBranches)
{
   TTree t("t", "t");
   // In order to reproduce the problem at #11207, we need to create a TTree with branch
   // "topbranch.something" where `something` must _not_ be a data member of the type
   // of "topbranch" (otherwise things "happen" to work due to the order in which we do substitutions in RDF, see
   // below).
   std::vector<ROOT::Math::XYZTVector> objs{{}, {}};

   t.Branch("topbranch", &objs);
   t.Fill();
   t.Fill();
   t.Fill();

#ifndef NDEBUG
   ROOT::TestSupport::CheckDiagsRAII diagRAII{kWarning, "RTreeColumnReader::Get",
                                              "Branch topbranch.fCoordinates.fX hangs from a non-split branch. A copy "
                                              "is being performed in order to properly read the content."};
#endif
   auto df = ROOT::RDataFrame(t).Alias("alias", "topbranch");
   // Here, before the fix for #11207 we transformed `"alias.fCoordinates.fX.size() == 2"` into
   // `[](std::vector<XYZTVector> &var0) { return var0.fCoordinates.fX.size() == 2; }`, which is not valid C++
   // (std::vector does not have a fCoordinates data member). Now the string expression is correctly expanded to
   // `[](RVec<typeof(XYZTVector{}.fCoordinates.fX)> &var0) { return var0.size() == 2; }`
   auto entryCount = df.Filter("alias.fCoordinates.fX.size() == 2").Count();
   EXPECT_EQ(*entryCount, 3);
}

TEST_P(RDFRegressionTests, MultipleTriggerRun)
{
   const auto fileName = std::string("dataframe_regression_0") + std::to_string(GetParam()) + ".root";
   const auto treeName = "t";
   {
      ROOT::RDataFrame tdf(1);
      tdf.Define("b1", []() { return 1U; }).Snapshot(treeName, fileName, {"b1"});
   }

   ROOT::RDataFrame d(treeName, fileName, {"b1"});
   int i = 0;
   auto sentinel = [&i]() {
      ++i;
      return true;
   };
   auto f1 = d.Filter(sentinel);
   auto m1 = f1.Min();
   *m1; // this should cause i to be incremented
   EXPECT_EQ(1, i) << "The filter was not correctly executed.";

   auto f2 = d.Filter(sentinel);
   auto dummy = f2.Max();
   *m1; // this should NOT cause i to be incremented
   EXPECT_EQ(1, i) << "The filter was executed also when it was not supposed to.";

   *dummy; // this should cause i to be incremented
   EXPECT_EQ(2, i) << "The filter was not correctly executed for the second time.";

   gSystem->Unlink(fileName.c_str());
}

TEST_P(RDFRegressionTests, EmptyTree)
{
   const auto fileName = std::string("dataframe_regression_2") + std::to_string(GetParam()) + ".root";
   const auto treeName = "t";
   {
      TFile wf(fileName.c_str(), "RECREATE");
      TTree t(treeName, treeName);
      int a;
      t.Branch("a", &a);
      t.Write();
   }

   ROOT::RDataFrame d(treeName, fileName, {"a"});
   auto min = d.Min<int>();
   auto max = d.Max<int>();
   auto mean = d.Mean<int>();
   auto h = d.Histo1D<int>();
   auto c = d.Count();
   auto g = d.Take<int>();
   std::atomic_int fc(0);
   d.Foreach([&fc]() { ++fc; });

   EXPECT_DOUBLE_EQ(*min, std::numeric_limits<int>::max());
   EXPECT_DOUBLE_EQ(*max, std::numeric_limits<int>::lowest());
   EXPECT_DOUBLE_EQ(*mean, 0);
   EXPECT_EQ(h->GetEntries(), 0);
   EXPECT_EQ(*c, 0U);
   EXPECT_EQ(g->size(), 0U);
   EXPECT_EQ(fc.load(), 0);

   gSystem->Unlink(fileName.c_str());
}

// check that rdfentry_ contains all expected values,
// also in multi-thread runs over multiple ROOT files
TEST_P(RDFRegressionTests, UniqueEntryNumbers)
{
   const auto treename = "t";
   const auto fname = "df_uniqueentrynumbers.root";
   ROOT::RDataFrame(10).Snapshot(treename, fname, {"rdfslot_"}); // does not matter what column we write

   ROOT::RDataFrame df(treename, {fname, fname});
   auto entries = *df.Take<ULong64_t>("rdfentry_");
   std::sort(entries.begin(), entries.end());
   const auto nEntries = entries.size();
   for (auto i = 0u; i < nEntries; ++i)
      EXPECT_EQ(i, entries[i]);

   gSystem->Unlink(fname);
}

// ROOT-9731
TEST_P(RDFRegressionTests, ReadWriteVector3)
{
   const std::string filename = "readwritetvector3.root";
   {
      TFile tfile(filename.c_str(), "recreate");
      TTree tree("t", "t");
      TVector3 a;
      tree.Branch("a", &a); // TVector3 as TBranchElement
      auto *c = new TVector3();
      tree.Branch("b", "TVector3", &c, 32000, 0); // TVector3 as TBranchObject
      for (int i = 0; i < 10; ++i) {
         a.SetX(i);
         c->SetX(i);
         tree.Fill();
      }
      tree.Write();
      delete c;
   }

   const std::string snap_fname = std::string("snap_") + filename;

   ROOT::RDataFrame rdf("t", filename);
   auto ha = rdf.Define("aval", "a.X()").Histo1D("aval");
   auto hb = rdf.Define("bval", "b.X()").Histo1D("bval");
   EXPECT_EQ(ha->GetMean(), 4.5);
   EXPECT_EQ(ha->GetMean(), hb->GetMean());

   auto out_df = rdf.Snapshot("t", snap_fname, {"a", "b"});

   auto ha_snap = out_df->Define("aval", "a.X()").Histo1D("aval");
   auto hb_snap = out_df->Define("bval", "b.X()").Histo1D("bval");
   EXPECT_EQ(ha_snap->GetMean(), 4.5);
   EXPECT_EQ(ha_snap->GetMean(), hb_snap->GetMean());

   gSystem->Unlink(snap_fname.c_str());
   gSystem->Unlink(filename.c_str());
}

TEST_P(RDFRegressionTests, PolymorphicTBranchObject)
{
   const std::string filename = "polymorphictbranchobject.root";
   {
      TFile f(filename.c_str(), "recreate");
      TTree t("t", "t");
      TObject *o = nullptr;
      t.Branch("o", &o, 32000, 0); // must be unsplit to generate a TBranchObject

      // Fill branch with different concrete types
      TNamed name("name", "title");
      TList list;
      list.Add(&name);
      o = &list;
      t.Fill();
      TH1D h("h", "h", 100, 0, 100);
      h.Fill(42);
      o = &h;
      t.Fill();
      o = nullptr;

      t.Write();
   }

   auto checkEntries = [](const TObject &obj, ULong64_t entry) {
      if (entry % 2 == 0) {
         EXPECT_STREQ(obj.ClassName(), "TList");
         auto &asList = dynamic_cast<const TList &>(obj);
         EXPECT_EQ(asList.GetEntries(), 1);
         EXPECT_STREQ(asList.At(0)->GetTitle(), "title");
         EXPECT_STREQ(asList.At(0)->GetName(), "name");
      } else {
         EXPECT_STREQ(obj.ClassName(), "TH1D");
         EXPECT_DOUBLE_EQ(dynamic_cast<const TH1D &>(obj).GetMean(), 42.);
      }
   };

   const std::string snap_fname = std::string("snap_") + filename;

   // Read in the same file twice to force the use of TChain,
   // which has trickier edge cases due to object addresses changing
   // when switching from one file to the other.
   ROOT::RDataFrame rdf("t", {filename, filename});
   ASSERT_EQ(rdf.Count().GetValue(), 4ull);
   rdf.Foreach(checkEntries, {"o", "rdfentry_"});

   auto out_df = rdf.Snapshot("t", snap_fname, {"o"});
   out_df->Foreach(checkEntries, {"o", "rdfentry_"});

   TFile f(snap_fname.c_str());
   auto t = f.Get<TTree>("t");
   EXPECT_EQ(t->GetBranch("o")->IsA(), TBranchObject::Class());

   gSystem->Unlink(snap_fname.c_str());
   gSystem->Unlink(filename.c_str());
}

// #11222
TEST_P(RDFRegressionTests, UseAfterDeleteOfSampleCallbacks)
{
   struct MyHelper : ROOT::Detail::RDF::RActionImpl<MyHelper> {
      using Result_t = int;
      std::shared_ptr<int> fResult;
      bool fThisWasDeleted = false;

      MyHelper() : fResult(std::make_shared<int>()) {}
      ~MyHelper() override { fThisWasDeleted = true; }
      void Initialize() {}
      void InitTask(TTreeReader *, unsigned int) {}
      void Exec(unsigned int) {}
      void Finalize() {}
      std::shared_ptr<Result_t> GetResultPtr() const { return fResult; }
      std::string GetActionName() const { return "MyHelper"; }
      ROOT::RDF::SampleCallback_t GetSampleCallback() final
      {
         // in a well-behaved program, this won't ever even be called as the RResultPtr returned
         // by the Book call below goes immediately out of scope, removing the action from the computation graph
         return [this](unsigned int, const ROOT::RDF::RSampleInfo &) { EXPECT_FALSE(fThisWasDeleted); };
      }
   };

   ROOT::RDataFrame df(10);
   // Book custom action and immediately deregister it (because the RResultPtr goes out of scope)
   df.Book<>(MyHelper{}, {});
   // trigger an event loop that will invoke registered sample callbacks (in a well-behaved program, none should run)
   df.Count().GetValue();
}

// #16475
struct DatasetGuard {
   DatasetGuard(std::string_view treeName, std::string_view fileName) : fTreeName(treeName), fFileName(fileName)
   {
      TFile f{fFileName.c_str(), "recreate"};
      TTree t{fTreeName.c_str(), fTreeName.c_str()};
      int x{};
      t.Branch("x", &x, "x/I");
      for (auto i = 0; i < 10; i++) {
         x = i;
         t.Fill();
      }
      f.Write();
   }
   DatasetGuard(const DatasetGuard &) = delete;
   DatasetGuard &operator=(const DatasetGuard &) = delete;
   DatasetGuard(DatasetGuard &&) = delete;
   DatasetGuard &operator=(DatasetGuard &&) = delete;
   ~DatasetGuard() { std::remove(fFileName.c_str()); }
   std::string fTreeName;
   std::string fFileName;
};

TEST_P(RDFRegressionTests, FileNameQuery)
{
   DatasetGuard dataset{"events", "dataframe_regression_filenamequery.root"};
   constexpr auto fileNameWithQuery{"dataframe_regression_filenamequery.root?myq=xyz"};
   ROOT::RDataFrame df{dataset.fTreeName, fileNameWithQuery};
   EXPECT_EQ(df.Count().GetValue(), 10);
}

TEST_P(RDFRegressionTests, FileNameWildcardQuery)
{
   DatasetGuard dataset{"events", "dataframe_regression_filenamewildcardquery.root"};
   constexpr auto fileNameWithQuery{"dataframe_regress?on_filenamewildcardquery.root?myq=xyz"};
   ROOT::RDataFrame df{dataset.fTreeName, fileNameWithQuery};
   EXPECT_EQ(df.Count().GetValue(), 10);
}

TEST_P(RDFRegressionTests, FileNameQueryNoExt)
{
   DatasetGuard dataset{"events", "dataframe_regression_filenamequerynoext"};
   constexpr auto fileNameWithQuery{"dataframe_regression_filenamequerynoext?myq=xyz"};
   ROOT::RDataFrame df{dataset.fTreeName, fileNameWithQuery};
   EXPECT_EQ(df.Count().GetValue(), 10);
}

TEST_P(RDFRegressionTests, EmptyFileList)
{
   try {
      ROOT::RDataFrame df{"", {}};
   } catch (const std::invalid_argument &e) {
      const std::string expected{"RDataFrame: empty list of input files."};
      EXPECT_EQ(e.what(), expected);
   }
}

// run single-thread tests
INSTANTIATE_TEST_SUITE_P(Seq, RDFRegressionTests, ::testing::Values(false));

// run multi-thread tests
#ifdef R__USE_IMT
   INSTANTIATE_TEST_SUITE_P(MT, RDFRegressionTests, ::testing::Values(true));
#endif
