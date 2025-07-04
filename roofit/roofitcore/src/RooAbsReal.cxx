/*****************************************************************************
 * Project: RooFit                                                           *
 * Package: RooFitCore                                                       *
 * @(#)root/roofitcore:$Id$
 * Authors:                                                                  *
 *   WV, Wouter Verkerke, UC Santa Barbara, verkerke@slac.stanford.edu       *
 *   DK, David Kirkby,    UC Irvine,         dkirkby@uci.edu                 *
 *                                                                           *
 * Copyright (c) 2000-2005, Regents of the University of California          *
 *                          and Stanford University. All rights reserved.    *
 *                                                                           *
 * Redistribution and use in source and binary forms,                        *
 * with or without modification, are permitted according to the terms        *
 * listed in LICENSE (http://roofit.sourceforge.net/license.txt)             *
 *****************************************************************************/

//////////////////////////////////////////////////////////////////////////////

/** \class RooAbsReal

   Abstract base class for objects that represent a
   real value and implements functionality common to all real-valued objects
   such as the ability to plot them, to construct integrals of them, the
   ability to advertise (partial) analytical integrals etc.

   Implementation of RooAbsReal may be derived, thus no interface
   is provided to modify the contents.

   \ingroup Roofitcore
*/

#include "RooAbsReal.h"

#include "FitHelpers.h"
#include "RooAbsCategoryLValue.h"
#include "RooAbsData.h"
#include "RooAddPdf.h"
#include "RooAddition.h"
#include "RooArgList.h"
#include "RooArgSet.h"
#include "RooBinning.h"
#include "RooBrentRootFinder.h"
#include "RooCachedReal.h"
#include "RooCategory.h"
#include "RooCmdConfig.h"
#include "RooConstVar.h"
#include "RooCurve.h"
#include "RooCustomizer.h"
#include "RooDataHist.h"
#include "RooDataSet.h"
#include "RooDerivative.h"
#include "RooFirstMoment.h"
#include "RooFit/BatchModeDataHelpers.h"
#include "RooFit/Evaluator.h"
#include "RooFitResult.h"
#include "RooFormulaVar.h"
#include "RooFunctor.h"
#include "RooGlobalFunc.h"
#include "RooFitImplHelpers.h"
#include "RooHist.h"
#include "RooMoment.h"
#include "RooMsgService.h"
#include "RooNumIntConfig.h"
#include "RooNumRunningInt.h"
#include "RooParamBinning.h"
#include "RooPlot.h"
#include "RooProduct.h"
#include "RooProfileLL.h"
#include "RooRealBinding.h"
#include "RooRealIntegral.h"
#include "RooRealVar.h"
#include "RooSecondMoment.h"
#include "RooVectorDataStore.h"
#include "TreeReadBuffer.h"
#include "ValueChecking.h"

#include "ROOT/StringUtils.hxx"
#include "Compression.h"
#include "Math/IFunction.h"
#include "TMath.h"
#include "TTree.h"
#include "TH1.h"
#include "TH2.h"
#include "TH3.h"
#include "TBranch.h"
#include "TLeaf.h"
#include "TAttLine.h"
#include "TF1.h"
#include "TF2.h"
#include "TF3.h"
#include "TMatrixD.h"
#include "TVector.h"
#include "strlcpy.h"
#ifndef NDEBUG
#include <TSystem.h> // To print stack traces when caching errors are detected
#endif

#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <sys/types.h>

namespace {

// Internal helper RooAbsFunc that evaluates the scaled data-weighted average of
// given RooAbsReal as a function of a single variable using the RooFit::Evaluator.
class ScaledDataWeightedAverage : public RooAbsFunc {
public:
   ScaledDataWeightedAverage(RooAbsReal const &arg, RooAbsData const &data, double scaleFactor, RooAbsRealLValue &var)
      : RooAbsFunc{1}, _var{var}, _dataWeights{data.getWeightBatch(0, data.numEntries())}, _scaleFactor{scaleFactor}
   {
      _arg = RooFit::Detail::compileForNormSet(arg, *data.get());
      _arg->recursiveRedirectServers(RooArgList{var});
      _evaluator = std::make_unique<RooFit::Evaluator>(*_arg);
      std::stack<std::vector<double>>{}.swap(_vectorBuffers);
      auto dataSpans = RooFit::BatchModeDataHelpers::getDataSpans(data, "", nullptr, /*skipZeroWeights=*/false,
                                                                   /*takeGlobalObservablesFromData=*/true,
                                                                   _vectorBuffers);
      for (auto const& item : dataSpans) {
         _evaluator->setInput(item.first->GetName(), item.second, false);
      }
   }

   double operator()(const double xvector[]) const override
   {
      double oldVal = _var.getVal();
      _var.setVal(xvector[0]);

      double out = 0.0;
      std::span<const double> pdfValues = _evaluator->run();
      if (_dataWeights.empty()) {
         out = std::accumulate(pdfValues.begin(), pdfValues.end(), 0.0) / pdfValues.size();
      } else {
         double weightsSum = 0.0;
         for (std::size_t i = 0; i < pdfValues.size(); ++i) {
            out += pdfValues[i] * _dataWeights[i];
            weightsSum += _dataWeights[i];
         }
         out /= weightsSum;
      }
      out *= _scaleFactor;

      _var.setVal(oldVal);
      return out;
   }
   double getMinLimit(UInt_t /*dimension*/) const override { return _var.getMin(); }
   double getMaxLimit(UInt_t /*dimension*/) const override { return _var.getMax(); }

private:
   RooAbsRealLValue &_var;
   std::unique_ptr<RooAbsReal> _arg;
   std::span<const double> _dataWeights;
   double _scaleFactor;
   std::unique_ptr<RooFit::Evaluator> _evaluator;
   std::stack<std::vector<double>> _vectorBuffers;
};

struct EvalErrorData {
   using ErrorList = std::map<const RooAbsArg *, std::pair<std::string, std::list<RooAbsReal::EvalError>>>;
   RooAbsReal::ErrorLoggingMode mode = RooAbsReal::PrintErrors;
   int count = 0;
   ErrorList errorList;
};

EvalErrorData &evalErrorData()
{
   static EvalErrorData data;
   return data;
}

} // namespace

Int_t RooAbsReal::numEvalErrorItems()
{
   return evalErrorData().errorList.size();
}

EvalErrorData::ErrorList::iterator RooAbsReal::evalErrorIter()
{
   return evalErrorData().errorList.begin();
}


bool RooAbsReal::_globalSelectComp = false;
bool RooAbsReal::_hideOffset = true ;

void RooAbsReal::setHideOffset(bool flag) { _hideOffset = flag ; }
bool RooAbsReal::hideOffset() { return _hideOffset ; }


////////////////////////////////////////////////////////////////////////////////
/// coverity[UNINIT_CTOR]
/// Default constructor

RooAbsReal::RooAbsReal() {}


////////////////////////////////////////////////////////////////////////////////
/// Constructor with unit label

RooAbsReal::RooAbsReal(const char *name, const char *title, const char *unit) : RooAbsReal{name, title, 0.0, 0.0, unit}
{
}


////////////////////////////////////////////////////////////////////////////////
/// Constructor with plot range and unit label

RooAbsReal::RooAbsReal(const char *name, const char *title, double inMinVal,
             double inMaxVal, const char *unit) :
  RooAbsArg(name,title), _plotMin(inMinVal), _plotMax(inMaxVal), _unit(unit)
{
  setValueDirty() ;
  setShapeDirty() ;
}


////////////////////////////////////////////////////////////////////////////////
/// Copy constructor
RooAbsReal::RooAbsReal(const RooAbsReal& other, const char* name) :
  RooAbsArg(other,name), _plotMin(other._plotMin), _plotMax(other._plotMax),
  _plotBins(other._plotBins), _value(other._value), _unit(other._unit), _label(other._label),
  _forceNumInt(other._forceNumInt), _selectComp(other._selectComp)
{
  if (other._specIntegratorConfig) {
    _specIntegratorConfig = std::make_unique<RooNumIntConfig>(*other._specIntegratorConfig) ;
  }
}


////////////////////////////////////////////////////////////////////////////////
/// Destructor

RooAbsReal::~RooAbsReal()
{
   if (_treeReadBuffer) {
      delete _treeReadBuffer;
   }
   _treeReadBuffer = nullptr;
}


////////////////////////////////////////////////////////////////////////////////
/// Equality operator comparing to a double

bool RooAbsReal::operator==(double value) const
{
  return (getVal()==value) ;
}



////////////////////////////////////////////////////////////////////////////////
/// Equality operator when comparing to another RooAbsArg.
/// Only functional when the other arg is a RooAbsReal

bool RooAbsReal::operator==(const RooAbsArg& other) const
{
  const RooAbsReal* otherReal = dynamic_cast<const RooAbsReal*>(&other) ;
  return otherReal ? operator==(otherReal->getVal()) : false ;
}


////////////////////////////////////////////////////////////////////////////////

bool RooAbsReal::isIdentical(const RooAbsArg& other, bool assumeSameType) const
{
  if (!assumeSameType) {
    const RooAbsReal* otherReal = dynamic_cast<const RooAbsReal*>(&other) ;
    return otherReal ? operator==(otherReal->getVal()) : false ;
  } else {
    return getVal() == static_cast<const RooAbsReal&>(other).getVal();
  }
}


////////////////////////////////////////////////////////////////////////////////
/// Return this variable's title string. If appendUnit is true and
/// this variable has units, also append a string " (<unit>)".

TString RooAbsReal::getTitle(bool appendUnit) const
{
  if(appendUnit && 0 != strlen(getUnit())) {
    return std::string{GetTitle()} + " (" + getUnit() + ")";
  }
  return GetTitle();
}



////////////////////////////////////////////////////////////////////////////////
/// Return value of object. If the cache is clean, return the
/// cached value, otherwise recalculate on the fly and refill
/// the cache

double RooAbsReal::getValV(const RooArgSet* nset) const
{
  if (nset && nset->uniqueId().value() != _lastNormSetId) {
    const_cast<RooAbsReal*>(this)->setProxyNormSet(nset);
    _lastNormSetId = nset->uniqueId().value();
  }

  if (isValueDirtyAndClear()) {
    _value = traceEval(nullptr) ;
    //     clearValueDirty() ;
  }

  return hideOffset() ? _value + offset() : _value;
}


////////////////////////////////////////////////////////////////////////////////
/// Calculate current value of object, with error tracing wrapper

double RooAbsReal::traceEval(const RooArgSet* /*nset*/) const
{
  double value = evaluate() ;

  if (TMath::IsNaN(value)) {
    logEvalError("function value is NAN") ;
  }

  //cxcoutD(Tracing) << "RooAbsReal::getValF(" << GetName() << ") operMode = " << _operMode << " recalculated, new value = " << value << std::endl ;

  //Standard tracing code goes here
  if (!isValidReal(value)) {
    coutW(Tracing) << "RooAbsReal::traceEval(" << GetName()
         << "): validation failed: " << value << std::endl ;
  }

  //Call optional subclass tracing code
  //   traceEvalHook(value) ;

  return value ;
}



////////////////////////////////////////////////////////////////////////////////
/// Variant of getAnalyticalIntegral that is also passed the normalization set
/// that should be applied to the integrand of which the integral is requested.
/// For certain operator p.d.f it is useful to overload this function rather
/// than analyticalIntegralWN() as the additional normalization information
/// may be useful in determining a more efficient decomposition of the
/// requested integral.

Int_t RooAbsReal::getAnalyticalIntegralWN(RooArgSet& allDeps, RooArgSet& analDeps,
                 const RooArgSet* /*normSet*/, const char* rangeName) const
{
  return _forceNumInt ? 0 : getAnalyticalIntegral(allDeps,analDeps,rangeName) ;
}



////////////////////////////////////////////////////////////////////////////////
/// Interface function getAnalyticalIntergral advertises the
/// analytical integrals that are supported. 'integSet'
/// is the set of dependents for which integration is requested. The
/// function should copy the subset of dependents it can analytically
/// integrate to anaIntSet and return a unique identification code for
/// this integration configuration.  If no integration can be
/// performed, zero should be returned.

Int_t RooAbsReal::getAnalyticalIntegral(RooArgSet& /*integSet*/, RooArgSet& /*anaIntSet*/, const char* /*rangeName*/) const
{
  return 0 ;
}



////////////////////////////////////////////////////////////////////////////////
/// Implements the actual analytical integral(s) advertised by
/// getAnalyticalIntegral.  This functions will only be called with
/// codes returned by getAnalyticalIntegral, except code zero.

double RooAbsReal::analyticalIntegralWN(Int_t code, const RooArgSet* normSet, const char* rangeName) const
{
//   std::cout << "RooAbsReal::analyticalIntegralWN(" << GetName() << ") code = " << code << " normSet = " << (normSet?*normSet:RooArgSet()) << std::endl ;
  if (code==0) return getVal(normSet) ;
  return analyticalIntegral(code,rangeName) ;
}



////////////////////////////////////////////////////////////////////////////////
/// Implements the actual analytical integral(s) advertised by
/// getAnalyticalIntegral.  This functions will only be called with
/// codes returned by getAnalyticalIntegral, except code zero.

double RooAbsReal::analyticalIntegral(Int_t code, const char* /*rangeName*/) const
{
  // By default no analytical integrals are implemented
  coutF(Eval)  << "RooAbsReal::analyticalIntegral(" << GetName() << ") code " << code << " not implemented" << std::endl ;
  return 0 ;
}



////////////////////////////////////////////////////////////////////////////////
/// Get the label associated with the variable

const char *RooAbsReal::getPlotLabel() const
{
  return _label.IsNull() ? fName.Data() : _label.Data();
}



////////////////////////////////////////////////////////////////////////////////
/// Set the label associated with this variable

void RooAbsReal::setPlotLabel(const char *label)
{
  _label= label;
}



////////////////////////////////////////////////////////////////////////////////
///Read object contents from stream (dummy for now)

bool RooAbsReal::readFromStream(std::istream& /*is*/, bool /*compact*/, bool /*verbose*/)
{
  return false ;
}



////////////////////////////////////////////////////////////////////////////////
///Write object contents to stream (dummy for now)

void RooAbsReal::writeToStream(std::ostream& /*os*/, bool /*compact*/) const
{
}



////////////////////////////////////////////////////////////////////////////////
/// Print object value

void RooAbsReal::printValue(std::ostream& os) const
{
  os << getVal() ;
}



////////////////////////////////////////////////////////////////////////////////
/// Structure printing

void RooAbsReal::printMultiline(std::ostream& os, Int_t contents, bool verbose, TString indent) const
{
  RooAbsArg::printMultiline(os,contents,verbose,indent) ;
  os << indent << "--- RooAbsReal ---" << std::endl;
  TString unit(_unit);
  if(!unit.IsNull()) unit.Prepend(' ');
  //os << indent << "  Value = " << getVal() << unit << std::endl;
  os << std::endl << indent << "  Plot label is \"" << getPlotLabel() << "\"" << "\n";
}


////////////////////////////////////////////////////////////////////////////////
/// Create a RooProfileLL object that eliminates all nuisance parameters in the
/// present function. The nuisance parameters are defined as all parameters
/// of the function except the stated paramsOfInterest

RooFit::OwningPtr<RooAbsReal> RooAbsReal::createProfile(const RooArgSet& paramsOfInterest)
{
  // Construct name of profile object
  auto name = std::string(GetName()) + "_Profile[";
  bool first = true;
  for (auto const& arg : paramsOfInterest) {
    if (first) {
      first = false ;
    } else {
      name.append(",") ;
    }
    name.append(arg->GetName()) ;
  }
  name.append("]") ;

  // Create and return profile object
  auto out = std::make_unique<RooProfileLL>(name.c_str(),(std::string("Profile of ") + GetTitle()).c_str(),*this,paramsOfInterest);
  return RooFit::makeOwningPtr<RooAbsReal>(std::move(out));
}






////////////////////////////////////////////////////////////////////////////////
/// Create an object that represents the integral of the function over one or more observables listed in `iset`.
/// The actual integration calculation is only performed when the returned object is evaluated. The name
/// of the integral object is automatically constructed from the name of the input function, the variables
/// it integrates and the range integrates over.
///
/// \note The integral over a PDF is usually not normalised (*i.e.*, it is usually not
/// 1 when integrating the PDF over the full range). In fact, this integral is used *to compute*
/// the normalisation of each PDF. See the [rf110 tutorial](group__tutorial__roofit.html)
/// for details on PDF normalisation.
///
/// The following named arguments are accepted
/// |  | Effect on integral creation
/// |--|-------------------------------
/// | `NormSet(const RooArgSet&)`            | Specify normalization set, mostly useful when working with PDFs
/// | `NumIntConfig(const RooNumIntConfig&)` | Use given configuration for any numeric integration, if necessary
/// | `Range(const char* name)`              | Integrate only over given range. Multiple ranges may be specified by passing multiple Range() arguments

RooFit::OwningPtr<RooAbsReal> RooAbsReal::createIntegral(const RooArgSet& iset, const RooCmdArg& arg1, const RooCmdArg& arg2,
                   const RooCmdArg& arg3, const RooCmdArg& arg4, const RooCmdArg& arg5,
                   const RooCmdArg& arg6, const RooCmdArg& arg7, const RooCmdArg& arg8) const
{
  // Define configuration for this method
  RooCmdConfig pc("RooAbsReal::createIntegral(" + std::string(GetName()) + ")");
  pc.defineString("rangeName","RangeWithName",0,"",true) ;
  pc.defineSet("normSet","NormSet",0,nullptr) ;
  pc.defineObject("numIntConfig","NumIntConfig",0,nullptr) ;

  // Process & check varargs
  pc.process(arg1,arg2,arg3,arg4,arg5,arg6,arg7,arg8) ;
  if (!pc.ok(true)) {
    return nullptr;
  }

  // Extract values from named arguments
  const char* rangeName = pc.getString("rangeName",nullptr,true) ;
  const RooArgSet* nset = pc.getSet("normSet",nullptr);
  const RooNumIntConfig* cfg = static_cast<const RooNumIntConfig*>(pc.getObject("numIntConfig",nullptr)) ;

  return createIntegral(iset,nset,cfg,rangeName) ;
}





////////////////////////////////////////////////////////////////////////////////
/// Create an object that represents the integral of the function over one or more observables listed in iset.
/// The actual integration calculation is only performed when the return object is evaluated. The name
/// of the integral object is automatically constructed from the name of the input function, the variables
/// it integrates and the range integrates over. If nset is specified the integrand is request
/// to be normalized over nset (only meaningful when the integrand is a pdf). If rangename is specified
/// the integral is performed over the named range, otherwise it is performed over the domain of each
/// integrated observable. If cfg is specified it will be used to configure any numeric integration
/// aspect of the integral. It will not force the integral to be performed numerically, which is
/// decided automatically by RooRealIntegral.

RooFit::OwningPtr<RooAbsReal> RooAbsReal::createIntegral(const RooArgSet& iset, const RooArgSet* nset,
                   const RooNumIntConfig* cfg, const char* rangeName) const
{
  if (!rangeName || strchr(rangeName,',')==nullptr) {
    // Simple case: integral over full range or single limited range
    return createIntObj(iset,nset,cfg,rangeName);
  }

  // Integral over multiple ranges
  std::vector<std::string> tokens = ROOT::Split(rangeName, ",");

  if(RooHelpers::checkIfRangesOverlap(iset, tokens)) {
    std::stringstream errMsg;
    errMsg << GetName() << " : integrating with respect to the variables " << iset << " on the ranges  \"" << rangeName
           << "\" is not possible because the ranges are overlapping";
    const std::string errMsgString = errMsg.str();
    coutE(Integration) << errMsgString << std::endl;
    throw std::invalid_argument(errMsgString);
  }

  RooArgSet components ;
  for (const std::string& token : tokens) {
    components.addOwned(std::unique_ptr<RooAbsReal>{createIntObj(iset,nset,cfg, token.c_str())});
  }

  const std::string title = std::string("Integral of ") + GetTitle();
  const std::string fullName = std::string(GetName()) + integralNameSuffix(iset,nset,rangeName).Data();

  auto out = std::make_unique<RooAddition>(fullName.c_str(), title.c_str(), components);
  out->addOwnedComponents(std::move(components));
  return RooFit::makeOwningPtr<RooAbsReal>(std::move(out));
}



////////////////////////////////////////////////////////////////////////////////
/// Internal utility function for createIntegral() that creates the actual integral object.
RooFit::OwningPtr<RooAbsReal> RooAbsReal::createIntObj(const RooArgSet& iset2, const RooArgSet* nset2,
                 const RooNumIntConfig* cfg, const char* rangeName) const
{
  // Make internal use copies of iset and nset
  RooArgSet iset(iset2) ;
  const RooArgSet* nset = nset2 ;


  // Initialize local variables perparing for recursive loop
  bool error = false ;
  const RooAbsReal* integrand = this ;
  std::unique_ptr<RooAbsReal> integral;

  // Handle trivial case of no integration here explicitly
  if (iset.empty()) {

    const std::string title = std::string("Integral of ") + GetTitle();
    const std::string name = std::string(GetName()) + integralNameSuffix(iset,nset,rangeName).Data();

    auto out = std::make_unique<RooRealIntegral>(name.c_str(), title.c_str(), *this, iset, nset, cfg, rangeName);
    return RooFit::makeOwningPtr<RooAbsReal>(std::move(out));
  }

  // Process integration over remaining integration variables
  while(!iset.empty()) {


    // Find largest set of observables that can be integrated in one go
    RooArgSet innerSet ;
    findInnerMostIntegration(iset,innerSet,rangeName) ;

    // If largest set of observables that can be integrated is empty set, problem was ill defined
    // Postpone error messaging and handling to end of function, exit loop here
    if (innerSet.empty()) {
      error = true ;
      break ;
    }

    // Prepare name and title of integral to be created
    const std::string title = std::string("Integral of ") + integrand->GetTitle();
    const std::string name = std::string(integrand->GetName()) + integrand->integralNameSuffix(innerSet,nset,rangeName).Data();

    std::unique_ptr<RooAbsReal> innerIntegral = std::move(integral);

    // Construct innermost integral
    integral = std::make_unique<RooRealIntegral>(name.c_str(),title.c_str(),*integrand,innerSet,nset,cfg,rangeName);

    // Integral of integral takes ownership of innermost integral
    if (innerIntegral) {
      integral->addOwnedComponents(std::move(innerIntegral));
    }

    // Remove already integrated observables from to-do list
    iset.remove(innerSet) ;

    // Send info message on recursion if needed
    if (integrand == this && !iset.empty()) {
      coutI(Integration) << GetName() << " : multidimensional integration over observables with parameterized ranges in terms of other integrated observables detected, using recursive integration strategy to construct final integral" << std::endl ;
    }

    // Prepare for recursion, next integral should integrate last integrand
    integrand = integral.get();


    // Only need normalization set in innermost integration
    nset = nullptr;
  }

  if (error) {
    coutE(Integration) << GetName() << " : ERROR while defining recursive integral over observables with parameterized integration ranges, please check that integration rangs specify uniquely defined integral " << std::endl;
    return nullptr;
  }


  // After-burner: apply interpolating cache on (numeric) integral if requested by user
  const char* cacheParamsStr = getStringAttribute("CACHEPARAMINT") ;
  if (cacheParamsStr && strlen(cacheParamsStr)) {

    std::unique_ptr<RooArgSet> intParams{integral->getVariables()};

    RooArgSet cacheParams = RooHelpers::selectFromArgSet(*intParams, cacheParamsStr);

    if (!cacheParams.empty()) {
      cxcoutD(Caching) << "RooAbsReal::createIntObj(" << GetName() << ") INFO: constructing " << cacheParams.size()
           << "-dim value cache for integral over " << iset2 << " as a function of " << cacheParams << " in range " << (rangeName?rangeName:"<none>") <<  std::endl ;
      std::string name = Form("%s_CACHE_[%s]",integral->GetName(),cacheParams.contentsString().c_str()) ;
      auto cachedIntegral = std::make_unique<RooCachedReal>(name.c_str(),name.c_str(),*integral,cacheParams);
      cachedIntegral->setInterpolationOrder(2) ;
      cachedIntegral->addOwnedComponents(std::move(integral));
      cachedIntegral->setCacheSource(true) ;
      if (integral->operMode()==ADirty) {
   cachedIntegral->setOperMode(ADirty) ;
      }
      //cachedIntegral->disableCache(true) ;
      return RooFit::makeOwningPtr<RooAbsReal>(std::move(cachedIntegral));
    }
  }

  return RooFit::makeOwningPtr(std::move(integral));
}



////////////////////////////////////////////////////////////////////////////////
/// Utility function for createIntObj() that aids in the construct of recursive integrals
/// over functions with multiple observables with parameterized ranges. This function
/// finds in a given set allObs over which integration is requested the largeset subset
/// of observables that can be integrated simultaneously. This subset consists of
/// observables with fixed ranges and observables with parameterized ranges whose
/// parameterization does not depend on any observable that is also integrated.

void RooAbsReal::findInnerMostIntegration(const RooArgSet& allObs, RooArgSet& innerObs, const char* rangeName) const
{
  // Make lists of
  // a) integrated observables with fixed ranges,
  // b) integrated observables with parameterized ranges depending on other integrated observables
  // c) integrated observables used in definition of any parameterized ranges of integrated observables
  RooArgSet obsWithFixedRange(allObs) ;
  RooArgSet obsWithParamRange ;
  RooArgSet obsServingAsRangeParams ;

  // Loop over all integrated observables
  for (const auto aarg : allObs) {
    // Check if observable is real-valued lvalue
    if (auto arglv = dynamic_cast<RooAbsRealLValue*>(aarg)) {

      // Check if range is parameterized
      RooAbsBinning& binning = arglv->getBinning(rangeName,false,true) ;
      if (binning.isParameterized()) {
        RooArgSet loBoundObs;
        RooArgSet hiBoundObs;
        binning.lowBoundFunc()->getObservables(&allObs, loBoundObs) ;
        binning.highBoundFunc()->getObservables(&allObs, hiBoundObs) ;

        // Check if range parameterization depends on other integrated observables
        if (loBoundObs.overlaps(allObs) || hiBoundObs.overlaps(allObs)) {
          obsWithParamRange.add(*aarg) ;
          obsWithFixedRange.remove(*aarg) ;
          obsServingAsRangeParams.add(loBoundObs,false) ;
          obsServingAsRangeParams.add(hiBoundObs,false) ;
        }
      }
    }
  }

  // Make list of fixed-range observables that are _not_ involved in the parameterization of ranges of other observables
  RooArgSet obsWithFixedRangeNP(obsWithFixedRange) ;
  obsWithFixedRangeNP.remove(obsServingAsRangeParams) ;

  // Make list of param-range observables that are _not_ involved in the parameterization of ranges of other observables
  RooArgSet obsWithParamRangeNP(obsWithParamRange) ;
  obsWithParamRangeNP.remove(obsServingAsRangeParams) ;

  // Construct inner-most integration: over observables (with fixed or param range) not used in any other param range definitions
  innerObs.removeAll() ;
  innerObs.add(obsWithFixedRangeNP) ;
  innerObs.add(obsWithParamRangeNP) ;

}


////////////////////////////////////////////////////////////////////////////////
/// Construct string with unique suffix name to give to integral object that encodes
/// integrated observables, normalization observables and the integration range name

TString RooAbsReal::integralNameSuffix(const RooArgSet& iset, const RooArgSet* nset, const char* rangeName, bool omitEmpty) const
{
  TString name ;
  if (!iset.empty()) {
    name.Append("_Int[" + RooHelpers::getColonSeparatedNameString(iset, ','));
    if (rangeName) {
      name.Append("|" + std::string{rangeName});
    }
    name.Append("]");
  } else if (!omitEmpty) {
    name.Append("_Int[]") ;
  }

  if (nset && !nset->empty()) {
    name.Append("_Norm[" + RooHelpers::getColonSeparatedNameString(*nset, ','));
    const RooAbsPdf* thisPdf = dynamic_cast<const RooAbsPdf*>(this) ;
    if (thisPdf && thisPdf->normRange()) {
      name.Append("|" + std::string{thisPdf->normRange()}) ;
    }
    name.Append("]") ;
  }

  return name ;
}



////////////////////////////////////////////////////////////////////////////////
/// Utility function for plotOn() that creates a projection of a function or p.d.f
/// to be plotted on a RooPlot.
/// \ref createPlotProjAnchor "createPlotProjection()"

const RooAbsReal* RooAbsReal::createPlotProjection(const RooArgSet& depVars, const RooArgSet& projVars,
                                               RooArgSet*& cloneSet) const
{
  return createPlotProjection(depVars,&projVars,cloneSet) ;
}


////////////////////////////////////////////////////////////////////////////////
/// Utility function for plotOn() that creates a projection of a function or p.d.f
/// to be plotted on a RooPlot.
/// \anchor createPlotProjAnchor
///
/// Create a new object \f$ G \f$ that represents the normalized projection:
/// \f[
///  G[x,p] = \frac{\int F[x,y,p] \; \mathrm{d}\{y\}}
///                {\int F[x,y,p] \; \mathrm{d}\{x\} \, \mathrm{d}\{y\}}
/// \f]
/// where \f$ F[x,y,p] \f$ is the function we represent, and
/// \f$ \{ p \} \f$ are the remaining variables ("parameters").
///
/// \param[in] dependentVars Dependent variables over which to normalise, \f$ \{x\} \f$.
/// \param[in] projectedVars Variables to project out, \f$ \{ y \} \f$.
/// \param[out] cloneSet Will be set to a RooArgSet*, which will contain a clone of *this plus its projection integral object.
/// The latter will also be returned. The caller takes ownership of this set.
/// \param[in] rangeName Optional range for projection integrals
/// \param[in] condObs Conditional observables, which are not integrated for normalisation, even if they
/// are in `dependentVars` or `projectedVars`.
/// \return A pointer to the newly created object, or zero in case of an
/// error. The caller is responsible for deleting the `cloneSet` (which includes the returned projection object).
const RooAbsReal *RooAbsReal::createPlotProjection(const RooArgSet &dependentVars, const RooArgSet *projectedVars,
                     RooArgSet *&cloneSet, const char* rangeName, const RooArgSet* condObs) const
{
  // Get the set of our leaf nodes
  RooArgSet leafNodes;
  RooArgSet treeNodes;
  leafNodeServerList(&leafNodes,this);
  treeNodeServerList(&treeNodes,this) ;


  // Check that the dependents are all fundamental. Filter out any that we
  // do not depend on, and make substitutions by name in our leaf list.
  // Check for overlaps with the projection variables.
  for (const auto arg : dependentVars) {
    if(!arg->isFundamental() && !dynamic_cast<const RooAbsLValue*>(arg)) {
      coutE(Plotting) << ClassName() << "::" << GetName() << ":createPlotProjection: variable \"" << arg->GetName()
          << "\" of wrong type: " << arg->ClassName() << std::endl;
      return nullptr;
    }

    RooAbsArg *found= treeNodes.find(arg->GetName());
    if(!found) {
      coutE(Plotting) << ClassName() << "::" << GetName() << ":createPlotProjection: \"" << arg->GetName()
                << "\" is not a dependent and will be ignored." << std::endl;
      continue;
    }
    if(found != arg) {
      if (leafNodes.find(found->GetName())) {
        leafNodes.replace(*found,*arg);
      } else {
        leafNodes.add(*arg) ;

        // Remove any dependents of found, replace by dependents of LV node
        RooArgSet lvDep;
        arg->getObservables(&leafNodes, lvDep);
        for (const auto lvs : lvDep) {
          RooAbsArg* tmp = leafNodes.find(lvs->GetName()) ;
          if (tmp) {
            leafNodes.remove(*tmp) ;
            leafNodes.add(*lvs) ;
          }
        }
      }
    }

    // check if this arg is also in the projection set
    if(nullptr != projectedVars && projectedVars->find(arg->GetName())) {
      coutE(Plotting) << ClassName() << "::" << GetName() << ":createPlotProjection: \"" << arg->GetName()
                << "\" cannot be both a dependent and a projected variable." << std::endl;
      return nullptr;
    }
  }

  // Remove the projected variables from the list of leaf nodes, if necessary.
  if(nullptr != projectedVars) leafNodes.remove(*projectedVars,true);

  // Make a deep-clone of ourself so later operations do not disturb our original state
  cloneSet = new RooArgSet;
  if (RooArgSet(*this).snapshot(*cloneSet, true)) {
    coutE(Plotting) << "RooAbsPdf::createPlotProjection(" << GetName() << ") Couldn't deep-clone PDF, abort," << std::endl ;
    return nullptr ;
  }
  RooAbsReal *theClone= static_cast<RooAbsReal*>(cloneSet->find(GetName()));

  // The remaining entries in our list of leaf nodes are the external
  // dependents (x) and parameters (p) of the projection. Patch them back
  // into the theClone. This orphans the nodes they replace, but the orphans
  // are still in the cloneList and so will be cleaned up eventually.
  //cout << "redirection leafNodes : " ; leafNodes.Print("1") ;

  std::unique_ptr<RooArgSet> plotLeafNodes{leafNodes.selectCommon(dependentVars)};
  theClone->recursiveRedirectServers(*plotLeafNodes,false,false,false);

  // Create the set of normalization variables to use in the projection integrand
  RooArgSet normSet(dependentVars);
  if(nullptr != projectedVars) normSet.add(*projectedVars);
  if(nullptr != condObs) {
    normSet.remove(*condObs,true,true) ;
  }

  // Try to create a valid projection integral. If no variables are to be projected,
  // create a null projection anyway to bind our normalization over the dependents
  // consistently with the way they would be bound with a non-trivial projection.
  RooArgSet empty;
  if(nullptr == projectedVars) projectedVars= &empty;

  std::string name = GetName();
  name += integralNameSuffix(*projectedVars,&normSet,rangeName,true) ;

  std::string title = std::string{"Projection of "} + GetTitle();

  std::unique_ptr<RooAbsReal> projected{theClone->createIntegral(*projectedVars,normSet,rangeName)};

  if(nullptr == projected || !projected->isValid()) {
    coutE(Plotting) << ClassName() << "::" << GetName() << ":createPlotProjection: cannot integrate out ";
    projectedVars->printStream(std::cout,kName|kArgs,kSingleLine);
    return nullptr;
  }

  if(projected->InheritsFrom(RooRealIntegral::Class())){
    static_cast<RooRealIntegral&>(*projected).setAllowComponentSelection(true);
  }

  projected->SetName(name.c_str()) ;
  projected->SetTitle(title.c_str()) ;

  // Add the projection integral to the cloneSet so that it eventually gets cleaned up by the caller.
  RooAbsReal *projectedPtr = projected.get();
  cloneSet->addOwned(std::move(projected));

  // return a const pointer to remind the caller that they do not delete the returned object
  // directly (it is contained in the cloneSet instead).
  return projectedPtr;
}



////////////////////////////////////////////////////////////////////////////////
/// Fill the ROOT histogram 'hist' with values sampled from this
/// function at the bin centers.  Our value is calculated by first
/// integrating out any variables in projectedVars and then scaling
/// the result by scaleFactor. Returns a pointer to the input
/// histogram, or zero in case of an error. The input histogram can
/// be any TH1 subclass, and therefore of arbitrary
/// dimension. Variables are matched with the (x,y,...) dimensions of
/// the input histogram according to the order in which they appear
/// in the input plotVars list. If scaleForDensity is true the
/// histogram is filled with a the functions density rather than
/// the functions value (i.e. the value at the bin center is multiplied
/// with bin volume)

TH1 *RooAbsReal::fillHistogram(TH1 *hist, const RooArgList &plotVars,
                double scaleFactor, const RooArgSet *projectedVars, bool scaleForDensity,
                const RooArgSet* condObs, bool setError) const
{
  // Do we have a valid histogram to use?
  if(nullptr == hist) {
    coutE(InputArguments) << ClassName() << "::" << GetName() << ":fillHistogram: no valid histogram to fill" << std::endl;
    return nullptr;
  }

  // Check that the number of plotVars matches the input histogram's dimension
  Int_t hdim= hist->GetDimension();
  if(hdim != int(plotVars.size())) {
    coutE(InputArguments) << ClassName() << "::" << GetName() << ":fillHistogram: plotVars has the wrong dimension" << std::endl;
    return nullptr;
  }


  // Check that the plot variables are all actually RooRealVars and print a warning if we do not
  // explicitly depend on one of them. Fill a set (not list!) of cloned plot variables.
  RooArgSet plotClones;
  for(std::size_t index= 0; index < plotVars.size(); index++) {
    const RooAbsArg *var= plotVars.at(index);
    const RooRealVar *realVar= dynamic_cast<const RooRealVar*>(var);
    if(nullptr == realVar) {
      coutE(InputArguments) << ClassName() << "::" << GetName() << ":fillHistogram: cannot plot variable \"" << var->GetName()
      << "\" of type " << var->ClassName() << std::endl;
      return nullptr;
    }
    if(!this->dependsOn(*realVar)) {
      coutE(InputArguments) << ClassName() << "::" << GetName()
      << ":fillHistogram: WARNING: variable is not an explicit dependent: " << realVar->GetName() << std::endl;
    }
    plotClones.addClone(*realVar,true); // do not complain about duplicates
  }

  // Reconnect all plotClones to each other, imported when plotting N-dim integrals with entangled parameterized ranges
  for(RooAbsArg * pc : plotClones) {
    pc->recursiveRedirectServers(plotClones,false,false,true) ;
  }

  // Call checkObservables
  RooArgSet allDeps(plotClones) ;
  if (projectedVars) {
    allDeps.add(*projectedVars) ;
  }
  if (checkObservables(&allDeps)) {
    coutE(InputArguments) << "RooAbsReal::fillHistogram(" << GetName() << ") error in checkObservables, abort" << std::endl ;
    return hist ;
  }

  // Create a standalone projection object to use for calculating bin contents
  RooArgSet *cloneSet = nullptr;
  const RooAbsReal *projected= createPlotProjection(plotClones,projectedVars,cloneSet,nullptr,condObs);

  cxcoutD(Plotting) << "RooAbsReal::fillHistogram(" << GetName() << ") plot projection object is " << projected->GetName() << std::endl ;

  // Prepare to loop over the histogram bins
  Int_t xbins(0);
  Int_t ybins(1);
  Int_t zbins(1);
  RooRealVar *xvar = nullptr;
  RooRealVar *yvar = nullptr;
  RooRealVar *zvar = nullptr;
  TAxis *xaxis = nullptr;
  TAxis *yaxis = nullptr;
  TAxis *zaxis = nullptr;
  switch(hdim) {
  case 3:
    zbins= hist->GetNbinsZ();
    zvar= dynamic_cast<RooRealVar*>(plotClones.find(plotVars.at(2)->GetName()));
    zaxis= hist->GetZaxis();
    assert(nullptr != zvar && nullptr != zaxis);
    // fall through to next case...
  case 2:
    ybins= hist->GetNbinsY();
    yvar= dynamic_cast<RooRealVar*>(plotClones.find(plotVars.at(1)->GetName()));
    yaxis= hist->GetYaxis();
    assert(nullptr != yvar && nullptr != yaxis);
    // fall through to next case...
  case 1:
    xbins= hist->GetNbinsX();
    xvar= dynamic_cast<RooRealVar*>(plotClones.find(plotVars.at(0)->GetName()));
    xaxis= hist->GetXaxis();
    assert(nullptr != xvar && nullptr != xaxis);
    break;
  default:
    coutE(InputArguments) << ClassName() << "::" << GetName() << ":fillHistogram: cannot fill histogram with "
           << hdim << " dimensions" << std::endl;
    break;
  }

  // Loop over the input histogram's bins and fill each one with our projection's
  // value, calculated at the center.
  RooAbsReal::setEvalErrorLoggingMode(RooAbsReal::CollectErrors) ;
  Int_t xbin(0);
  Int_t ybin(0);
  Int_t zbin(0);
  Int_t bins= xbins*ybins*zbins;
  for(Int_t bin= 0; bin < bins; bin++) {
    switch(hdim) {
    case 3:
      if(bin % (xbins*ybins) == 0) {
   zbin++;
   zvar->setVal(zaxis->GetBinCenter(zbin));
      }
      // fall through to next case...
    case 2:
      if(bin % xbins == 0) {
   ybin= (ybin%ybins) + 1;
   yvar->setVal(yaxis->GetBinCenter(ybin));
      }
      // fall through to next case...
    case 1:
      xbin= (xbin%xbins) + 1;
      xvar->setVal(xaxis->GetBinCenter(xbin));
      break;
    default:
      coutE(InputArguments) << "RooAbsReal::fillHistogram: Internal Error!" << std::endl;
      break;
    }

    // Bin volume scaling
    double scaleFactorBin = scaleFactor;
    scaleFactorBin *= scaleForDensity && hdim > 2 ? hist->GetZaxis()->GetBinWidth(zbin) : 1.0;
    scaleFactorBin *= scaleForDensity && hdim > 1 ? hist->GetYaxis()->GetBinWidth(ybin) : 1.0;
    scaleFactorBin *= scaleForDensity && hdim > 0 ? hist->GetXaxis()->GetBinWidth(xbin) : 1.0;

    double result= scaleFactorBin * projected->getVal();
    if (RooAbsReal::numEvalErrors()>0) {
      coutW(Plotting) << "WARNING: Function evaluation error(s) at coordinates [x]=" << xvar->getVal() ;
      if (hdim==2) ccoutW(Plotting) << " [y]=" << yvar->getVal() ;
      if (hdim==3) ccoutW(Plotting) << " [z]=" << zvar->getVal() ;
      ccoutW(Plotting) << std::endl ;
      // RooAbsReal::printEvalErrors(ccoutW(Plotting),10) ;
      result = 0 ;
    }
    RooAbsReal::clearEvalErrorLog() ;

    hist->SetBinContent(hist->GetBin(xbin,ybin,zbin),result);
    if (setError) {
      hist->SetBinError(hist->GetBin(xbin,ybin,zbin),sqrt(result)) ;
    }

    //cout << "bin " << bin << " -> (" << xbin << "," << ybin << "," << zbin << ") = " << result << std::endl;
  }
  RooAbsReal::setEvalErrorLoggingMode(RooAbsReal::PrintErrors) ;

  // cleanup
  delete cloneSet;

  return hist;
}



////////////////////////////////////////////////////////////////////////////////
/// Fill a RooDataHist with values sampled from this function at the
/// bin centers.  If extendedMode is true, the p.d.f. values is multiplied
/// by the number of expected events in each bin
///
/// An optional scaling by a given scaleFactor can be performed.
/// Returns a pointer to the input RooDataHist, or zero
/// in case of an error.
///
/// If correctForBinSize is true the RooDataHist
/// is filled with the functions density (function value times the
/// bin volume) rather than function value.
///
/// If showProgress is true
/// a process indicator is printed on stdout in steps of one percent,
/// which is mostly useful for the sampling of expensive functions
/// such as likelihoods

RooDataHist* RooAbsReal::fillDataHist(RooDataHist *hist, const RooArgSet* normSet, double scaleFactor,
                  bool correctForBinSize, bool showProgress) const
{
  // Do we have a valid histogram to use?
  if(nullptr == hist) {
    coutE(InputArguments) << ClassName() << "::" << GetName() << ":fillDataHist: no valid RooDataHist to fill" << std::endl;
    return nullptr;
  }

  // Call checkObservables
  RooArgSet allDeps(*hist->get()) ;
  if (checkObservables(&allDeps)) {
    coutE(InputArguments) << "RooAbsReal::fillDataHist(" << GetName() << ") error in checkObservables, abort" << std::endl ;
    return hist ;
  }

  // Make deep clone of self and attach to dataset observables
  //RooArgSet* origObs = getObservables(hist) ;
  RooArgSet cloneSet;
  RooArgSet(*this).snapshot(cloneSet, true);
  RooAbsReal* theClone = static_cast<RooAbsReal*>(cloneSet.find(GetName()));
  theClone->recursiveRedirectServers(*hist->get()) ;
  //const_cast<RooAbsReal*>(this)->recursiveRedirectServers(*hist->get()) ;

  // Iterator over all bins of RooDataHist and fill weights
  Int_t onePct = hist->numEntries()/100 ;
  if (onePct==0) {
    onePct++ ;
  }
  for (Int_t i=0 ; i<hist->numEntries() ; i++) {
    if (showProgress && (i%onePct==0)) {
      ccoutP(Eval) << "." << std::flush ;
    }
    const RooArgSet* obs = hist->get(i) ;
    double binVal = theClone->getVal(normSet?normSet:obs)*scaleFactor ;
    if (correctForBinSize) {
      binVal*= hist->binVolume() ;
    }
    hist->set(i, binVal, 0.);
  }

  return hist;
}




////////////////////////////////////////////////////////////////////////////////
/// Create and fill a ROOT histogram TH1, TH2 or TH3 with the values of this function for the variables with given names.
/// \param[in] varNameList List of variables to use for x, y, z axis, separated by ':'
/// \param[in] xbins Number of bins for first variable
/// \param[in] ybins Number of bins for second variable
/// \param[in] zbins Number of bins for third variable
/// \return TH1*, which is one of TH[1-3]. The histogram is owned by the caller.
///
/// For a greater degree of control use
/// RooAbsReal::createHistogram(const char *, const RooAbsRealLValue&, const RooCmdArg&, const RooCmdArg&, const RooCmdArg&, const RooCmdArg&, const RooCmdArg&, const RooCmdArg&, const RooCmdArg&, const RooCmdArg&) const
///

TH1* RooAbsReal::createHistogram(RooStringView varNameList, Int_t xbins, Int_t ybins, Int_t zbins) const
{
  std::unique_ptr<RooArgSet> vars{getVariables()};

  auto varNames = ROOT::Split(varNameList, ",:");
  std::vector<RooRealVar*> histVars(3, nullptr);

  for(std::size_t iVar = 0; iVar < varNames.size(); ++iVar) {
    if(varNames[iVar].empty()) continue;
    if(iVar >= 3) {
      std::stringstream errMsg;
      errMsg << "RooAbsPdf::createHistogram(" << GetName() << ") ERROR more than three variable names passed, but maximum number of supported variables is three";
      coutE(Plotting) << errMsg.str() << std::endl;
      throw std::invalid_argument(errMsg.str());
    }
    auto var = static_cast<RooRealVar*>(vars->find(varNames[iVar].c_str()));
    if(!var) {
      std::stringstream errMsg;
      errMsg << "RooAbsPdf::createHistogram(" << GetName() << ") ERROR variable " << varNames[iVar] << " does not exist in argset: " << *vars;
      coutE(Plotting) << errMsg.str() << std::endl;
      throw std::runtime_error(errMsg.str());
    }
    histVars[iVar] = var;
  }

  // Construct list of named arguments to pass to the implementation version of createHistogram()

  RooLinkedList argList ;
  if (xbins>0) {
    argList.Add(RooFit::Binning(xbins).Clone()) ;
  }

  if (histVars[1]) {
    argList.Add(RooFit::YVar(*histVars[1], ybins > 0 ? RooFit::Binning(ybins) : RooCmdArg::none()).Clone()) ;
  }

  if (histVars[2]) {
    argList.Add(RooFit::ZVar(*histVars[2], zbins > 0 ? RooFit::Binning(zbins) : RooCmdArg::none()).Clone()) ;
  }

  // Call implementation function
  TH1* result = createHistogram(GetName(), *histVars[0], argList) ;

  // Delete temporary list of RooCmdArgs
  argList.Delete() ;

  return result ;
}


#define CREATE_CMD_LIST     \
   RooLinkedList l;         \
   l.Add((TObject *)&arg1); \
   l.Add((TObject *)&arg2); \
   l.Add((TObject *)&arg3); \
   l.Add((TObject *)&arg4); \
   l.Add((TObject *)&arg5); \
   l.Add((TObject *)&arg6); \
   l.Add((TObject *)&arg7); \
   l.Add((TObject *)&arg8);


////////////////////////////////////////////////////////////////////////////////
/// Create and fill a ROOT histogram TH1, TH2 or TH3 with the values of this function.
///
/// \param[in] name  Name of the ROOT histogram
/// \param[in] xvar  Observable to be std::mapped on x axis of ROOT histogram
/// \param[in] arg1,arg2,arg3,arg4,arg5,arg6,arg7,arg8  Arguments according to list below
/// \return TH1 *, one of TH{1,2,3}. The caller takes ownership.
///
/// <table>
/// <tr><th><th> Effect on histogram creation
/// <tr><td> `IntrinsicBinning()`                           <td> Apply binning defined by function or pdf (as advertised via binBoundaries() method)
/// <tr><td> `Binning(const char* name)`                    <td> Apply binning with given name to x axis of histogram
/// <tr><td> `Binning(RooAbsBinning& binning)`              <td> Apply specified binning to x axis of histogram
/// <tr><td> `Binning(int nbins, [double lo, double hi])`   <td> Apply specified binning to x axis of histogram
/// <tr><td> `ConditionalObservables(Args_t &&... argsOrArgSet)` <td> Do not normalise PDF over following observables when projecting PDF into histogram.
//                                                               Arguments can either be multiple RooRealVar or a single RooArgSet containing them.
/// <tr><td> `Scaling(bool)`                              <td> Apply density-correction scaling (multiply by bin volume), default is true
/// <tr><td> `Extended(bool)`                             <td> Plot event yield instead of probability density (for extended pdfs only)
///
/// <tr><td> `YVar(const RooAbsRealLValue& var,...)`    <td> Observable to be std::mapped on y axis of ROOT histogram.
/// The YVar() and ZVar() arguments can be supplied with optional Binning() arguments to control the binning of the Y and Z axes, e.g.
/// ```
/// createHistogram("histo",x,Binning(-1,1,20), YVar(y,Binning(-1,1,30)), ZVar(z,Binning("zbinning")))
/// ```
/// <tr><td> `ZVar(const RooAbsRealLValue& var,...)`    <td> Observable to be std::mapped on z axis of ROOT histogram
/// </table>
///
///

TH1 *RooAbsReal::createHistogram(const char *name, const RooAbsRealLValue& xvar,
             const RooCmdArg& arg1, const RooCmdArg& arg2, const RooCmdArg& arg3, const RooCmdArg& arg4,
             const RooCmdArg& arg5, const RooCmdArg& arg6, const RooCmdArg& arg7, const RooCmdArg& arg8) const
{
  CREATE_CMD_LIST;
  return createHistogram(name,xvar,l) ;
}


////////////////////////////////////////////////////////////////////////////////
/// Internal method implementing createHistogram

TH1* RooAbsReal::createHistogram(const char *name, const RooAbsRealLValue& xvar, RooLinkedList& argList) const
{

  // Define configuration for this method
  RooCmdConfig pc("RooAbsReal::createHistogram(" + std::string(GetName()) + ")");
  pc.defineInt("scaling","Scaling",0,1) ;
  pc.defineInt("intBinning","IntrinsicBinning",0,2) ;
  pc.defineInt("extended","Extended",0,2) ;

  pc.defineSet("compSet","SelectCompSet",0);
  pc.defineString("compSpec","SelectCompSpec",0) ;
  pc.defineSet("projObs","ProjectedObservables",0,nullptr) ;
  pc.defineObject("yvar","YVar",0,nullptr) ;
  pc.defineObject("zvar","ZVar",0,nullptr) ;
  pc.defineMutex("SelectCompSet","SelectCompSpec") ;
  pc.defineMutex("IntrinsicBinning","Binning") ;
  pc.defineMutex("IntrinsicBinning","BinningName") ;
  pc.defineMutex("IntrinsicBinning","BinningSpec") ;
  pc.allowUndefined() ;

  // Process & check varargs
  pc.process(argList) ;
  if (!pc.ok(true)) {
    return nullptr ;
  }

  RooArgList vars(xvar) ;
  RooAbsArg* yvar = static_cast<RooAbsArg*>(pc.getObject("yvar")) ;
  if (yvar) {
    vars.add(*yvar) ;
  }
  RooAbsArg* zvar = static_cast<RooAbsArg*>(pc.getObject("zvar")) ;
  if (zvar) {
    vars.add(*zvar) ;
  }

  auto projObs = pc.getSet("projObs");
  RooArgSet* intObs = nullptr ;

  bool doScaling = pc.getInt("scaling") ;
  Int_t doIntBinning = pc.getInt("intBinning") ;
  Int_t doExtended = pc.getInt("extended") ;

  // If doExtended is two, selection is automatic, set to 1 of pdf is extended, to zero otherwise
  const RooAbsPdf* pdfSelf = dynamic_cast<const RooAbsPdf*>(this) ;
  if (!pdfSelf && doExtended == 1) {
    coutW(InputArguments) << "RooAbsReal::createHistogram(" << GetName() << ") WARNING extended mode requested for a non-pdf object, ignored" << std::endl ;
    doExtended=0 ;
  }
  if (pdfSelf && doExtended==1 && pdfSelf->extendMode()==RooAbsPdf::CanNotBeExtended) {
    coutW(InputArguments) << "RooAbsReal::createHistogram(" << GetName() << ") WARNING extended mode requested for a non-extendable pdf, ignored" << std::endl ;
    doExtended=0 ;
  }
  if (pdfSelf && doExtended==2) {
    doExtended = pdfSelf->extendMode()==RooAbsPdf::CanNotBeExtended ? 0 : 1 ;
  } else if(!pdfSelf) {
    doExtended = 0;
  }

  const char* compSpec = pc.getString("compSpec") ;
  const RooArgSet* compSet = pc.getSet("compSet");
  bool haveCompSel = ( (compSpec && strlen(compSpec)>0) || compSet) ;

  std::unique_ptr<RooBinning> intBinning;
  if (doIntBinning>0) {
    // Given RooAbsPdf* pdf and RooRealVar* obs
    std::unique_ptr<std::list<double>> bl{binBoundaries(const_cast<RooAbsRealLValue&>(xvar),xvar.getMin(),xvar.getMax())};
    if (!bl) {
      // Only emit warning when intrinsic binning is explicitly requested
      if (doIntBinning==1) {
   coutW(InputArguments) << "RooAbsReal::createHistogram(" << GetName()
               << ") WARNING, intrinsic model binning requested for histogram, but model does not define bin boundaries, reverting to default binning"<< std::endl ;
      }
    } else {
      if (doIntBinning==2) {
   coutI(InputArguments) << "RooAbsReal::createHistogram(" << GetName()
               << ") INFO: Model has intrinsic binning definition, selecting that binning for the histogram"<< std::endl ;
      }
      std::vector<double> edges(bl->size());
      int i=0 ;
      for (auto const& elem : *bl) { edges[i++] = elem ; }
      intBinning = std::make_unique<RooBinning>(bl->size()-1,edges.data()) ;
    }
  }

  RooLinkedList argListCreate(argList) ;
  RooCmdConfig::stripCmdList(argListCreate,"Scaling,ProjectedObservables,IntrinsicBinning,SelectCompSet,SelectCompSpec,Extended") ;

  TH1* histo(nullptr) ;
  if (intBinning) {
    RooCmdArg tmp = RooFit::Binning(*intBinning) ;
    argListCreate.Add(&tmp) ;
    histo = xvar.createHistogram(name,argListCreate) ;
  } else {
    histo = xvar.createHistogram(name,argListCreate) ;
  }

  // Do component selection here
  if (haveCompSel) {

    // Get complete set of tree branch nodes
    RooArgSet branchNodeSet ;
    branchNodeServerList(&branchNodeSet) ;

    // Discard any non-RooAbsReal nodes
    for(RooAbsArg * arg : branchNodeSet) {
      if (!dynamic_cast<RooAbsReal*>(arg)) {
   branchNodeSet.remove(*arg) ;
      }
    }

    std::unique_ptr<RooArgSet> dirSelNodes;
    if (compSet) {
      dirSelNodes = std::unique_ptr<RooArgSet>{branchNodeSet.selectCommon(*compSet)};
    } else {
      dirSelNodes = std::unique_ptr<RooArgSet>{branchNodeSet.selectByName(compSpec)};
    }
    if (!dirSelNodes->empty()) {
      coutI(Plotting) << "RooAbsPdf::createHistogram(" << GetName() << ") directly selected PDF components: " << *dirSelNodes << std::endl ;

      // Do indirect selection and activate both
      plotOnCompSelect(dirSelNodes.get()) ;
    } else {
      if (compSet) {
   coutE(Plotting) << "RooAbsPdf::createHistogram(" << GetName() << ") ERROR: component selection set " << *compSet << " does not match any components of p.d.f." << std::endl ;
      } else {
   coutE(Plotting) << "RooAbsPdf::createHistogram(" << GetName() << ") ERROR: component selection expression '" << compSpec << "' does not select any components of p.d.f." << std::endl ;
      }
      return nullptr ;
    }
  }

  double scaleFactor(1.0) ;
  if (doExtended) {
     scaleFactor = pdfSelf->expectedEvents(vars);
  }

  fillHistogram(histo,vars,scaleFactor,intObs,doScaling,projObs,false) ;

  // Deactivate component selection
  if (haveCompSel) {
      plotOnCompSelect(nullptr) ;
  }


  return histo ;
}


////////////////////////////////////////////////////////////////////////////////
/// Helper function for plotting of composite p.d.fs. Given
/// a set of selected components that should be plotted,
/// find all nodes that (in)directly depend on these selected
/// nodes. Mark all directly and indirectly selected nodes
/// as 'selected' using the selectComp() method

void RooAbsReal::plotOnCompSelect(RooArgSet* selNodes) const
{
  // Get complete set of tree branch nodes
  RooArgSet branchNodeSet;
  branchNodeServerList(&branchNodeSet);

  // Discard any non-PDF nodes
  // Iterate by number because collection is being modified! Iterators may invalidate ...
  for (unsigned int i = 0; i < branchNodeSet.size(); ++i) {
    const auto arg = branchNodeSet[i];
    if (!dynamic_cast<RooAbsReal*>(arg)) {
      branchNodeSet.remove(*arg) ;
    }
  }

  // If no set is specified, restored all selection bits to true
  if (!selNodes) {
    // Reset PDF selection bits to true
    for (const auto arg : branchNodeSet) {
      static_cast<RooAbsReal*>(arg)->selectComp(true);
    }
    return ;
  }


  // Add all nodes below selected nodes that are value servers
  RooArgSet tmp;
  for (const auto arg : branchNodeSet) {
    for (const auto selNode : *selNodes) {
      if (selNode->dependsOn(*arg, nullptr, /*valueOnly=*/true)) {
        tmp.add(*arg,true);
      }
    }
  }

  // Add all nodes that depend on selected nodes by value
  for (const auto arg : branchNodeSet) {
    if (arg->dependsOn(*selNodes, nullptr, /*valueOnly=*/true)) {
      tmp.add(*arg,true);
    }
  }

  tmp.remove(*selNodes, true);
  tmp.remove(*this);
  selNodes->add(tmp);
  coutI(Plotting) << "RooAbsPdf::plotOn(" << GetName() << ") indirectly selected PDF components: " << tmp << std::endl ;

  // Set PDF selection bits according to selNodes
  for (const auto arg : branchNodeSet) {
    bool select = selNodes->find(arg->GetName()) != nullptr;
    static_cast<RooAbsReal*>(arg)->selectComp(select);
  }
}



////////////////////////////////////////////////////////////////////////////////
/// Plot (project) PDF on specified frame. If a PDF is plotted in an empty frame, it
/// will show a unit normalized curve in the frame variable, taken at the present value
/// of other observables defined for this PDF.
///
/// \param[in] frame pointer to RooPlot
/// \param[in] arg1,arg2,arg3,arg4,arg5,arg6,arg7,arg8,arg9,arg10 Ordered arguments
///
/// If a PDF is plotted in a frame in which a dataset has already been plotted, it will
/// show a projected curve integrated over all variables that were present in the shown
/// dataset except for the one on the x-axis. The normalization of the curve will also
/// be adjusted to the event count of the plotted dataset. An informational message
/// will be printed for each projection step that is performed.
///
/// This function takes the following named arguments
/// <table>
/// <tr><th><th> Projection control
/// <tr><td> `Slice(const RooArgSet& set)`     <td> Override default projection behaviour by omitting observables listed
///                                    in set from the projection, i.e. by not integrating over these.
///                                    Slicing is usually only sensible in discrete observables, by e.g. creating a slice
///                                    of the PDF at the current value of the category observable.
///
/// <tr><td> `Slice(RooCategory& cat, const char* label)`        <td> Override default projection behaviour by omitting the specified category
///                                    observable from the projection, i.e., by not integrating over all states of this category.
///                                    The slice is positioned at the given label value. To pass multiple Slice() commands, please use the
///                                    Slice(std::map<RooCategory*, std::string> const&) argument explained below.
///
/// <tr><td> `Slice(std::map<RooCategory*, std::string> const&)`        <td> Omits multiple categories from the projection, as explianed above.
///                                    Can be used with initializer lists for convenience, e.g.
/// ```{.cpp}
///   pdf.plotOn(frame, Slice({{&tagCategory, "2tag"}, {&jetCategory, "3jet"}});
/// ```
///
/// <tr><td> `Project(const RooArgSet& set)`   <td> Override default projection behaviour by projecting over observables
///                                    given in the set, ignoring the default projection behavior. Advanced use only.
///
/// <tr><td> `ProjWData(const RooAbsData& d)`  <td> Override default projection _technique_ (integration). For observables present in given dataset
///                                    projection of PDF is achieved by constructing an average over all observable values in given set.
///                                    Consult RooFit plotting tutorial for further explanation of meaning & use of this technique
///
/// <tr><td> `ProjWData(const RooArgSet& s, const RooAbsData& d)`   <td> As above but only consider subset 's' of observables in dataset 'd' for projection through data averaging
///
/// <tr><td> `ProjectionRange(const char* rn)` <td> Override default range of projection integrals to a different range specified by given range name.
///                                    This technique allows you to project a finite width slice in a real-valued observable
///
/// <tr><td> `NumCPU(Int_t ncpu)`              <td> Number of CPUs to use simultaneously to calculate data-weighted projections (only in combination with ProjWData)
///
///
/// <tr><th><th> Misc content control
/// <tr><td> `PrintEvalErrors(Int_t numErr)`   <td> Control number of p.d.f evaluation errors printed per curve. A negative
///                                    value suppress output completely, a zero value will only print the error count per p.d.f component,
///                                    a positive value is will print details of each error up to numErr messages per p.d.f component.
///
/// <tr><td> `EvalErrorValue(double value)`  <td> Set curve points at which (pdf) evaluation errors occur to specified value. By default the
///                                    function value is plotted.
///
/// <tr><td> `Normalization(double scale, ScaleType code)`   <td> Adjust normalization by given scale factor. Interpretation of number depends on code:
///                    - Relative: relative adjustment factor for a normalized function,
///                    - NumEvent: scale to match given number of events.
///                    - Raw: relative adjustment factor for an un-normalized function.
///
/// <tr><td> `Name(const chat* name)`          <td> Give curve specified name in frame. Useful if curve is to be referenced later
///
/// <tr><td> `Asymmetry(const RooCategory& c)` <td> Show the asymmetry of the PDF in given two-state category [F(+)-F(-)] / [F(+)+F(-)] rather than
///                                    the PDF projection. Category must have two states with indices -1 and +1 or three states with
///                                    indices -1,0 and +1.
///
/// <tr><td> `ShiftToZero(bool flag)`        <td> Shift entire curve such that lowest visible point is at exactly zero. Mostly useful when plotting \f$ -\log(L) \f$ or \f$ \chi^2 \f$ distributions
///
/// <tr><td> `AddTo(const char* name, double_t wgtSelf, double_t wgtOther)`   <td> Add constructed projection to already existing curve with given name and relative weight factors
/// <tr><td> `Components(const char* names)`  <td>  When plotting sums of PDFs, plot only the named components (*e.g.* only
///                                                 the signal of a signal+background model).
/// <tr><td> `Components(const RooArgSet& compSet)` <td> As above, but pass a RooArgSet of the components themselves.
///
/// <tr><th><th> Plotting control
/// <tr><td> `DrawOption(const char* opt)`     <td> Select ROOT draw option for resulting TGraph object. Currently supported options are "F" (fill), "L" (line), and "P" (points).
///           \note Option "P" will cause RooFit to plot (and treat) this pdf as if it were data! This is intended for plotting "corrected data"-type pdfs such as "data-minus-background" or unfolded datasets.
///
/// <tr><td> `LineStyle(Int_t style)`          <td> Select line style by ROOT line style code, default is solid
///
/// <tr><td> `LineColor(Int_t color)`          <td> Select line color by ROOT color code, default is blue
///
/// <tr><td> `LineWidth(Int_t width)`          <td> Select line with in pixels, default is 3
///
/// <tr><td> `MarkerStyle(Int_t style)`   <td> Select the ROOT marker style, default is 21
///
/// <tr><td> `MarkerColor(Int_t color)`   <td> Select the ROOT marker color, default is black
///
/// <tr><td> `MarkerSize(double size)`   <td> Select the ROOT marker size
///
/// <tr><td> `FillStyle(Int_t style)`          <td> Select fill style, default is not filled. If a filled style is selected, also use VLines()
///                                    to add vertical downward lines at end of curve to ensure proper closure. Add `DrawOption("F")` for filled drawing.
/// <tr><td> `FillColor(Int_t color)`          <td> Select fill color by ROOT color code
///
/// <tr><td> `Range(const char* name)`         <td> Only draw curve in range defined by given name
///
/// <tr><td> `Range(double lo, double hi)`     <td> Only draw curve in specified range
///
/// <tr><td> `VLines()`                        <td> Add vertical lines to y=0 at end points of curve
///
/// <tr><td> `Precision(double eps)`         <td> Control precision of drawn curve w.r.t to scale of plot, default is 1e-3. Higher precision
///                                    will result in more and more densely spaced curve points
///
/// <tr><td> `Invisible(bool flag)`           <td> Add curve to frame, but do not display. Useful in combination AddTo()
///
/// <tr><td> `VisualizeError(const RooFitResult& fitres, double Z=1, bool linearMethod=true)`
///                                  <td> Visualize the uncertainty on the parameters, as given in fitres, at 'Z' sigma'. The linear method is fast but may not be accurate in the presence of strong correlations (~>0.9) and at Z>2 due to linear and Gaussian approximations made. Intervals from the sampling method can be asymmetric, and may perform better in the presence of strong correlations, but may take (much) longer to calculate
///
/// <tr><td> `VisualizeError(const RooFitResult& fitres, const RooArgSet& param, double Z=1, bool linearMethod=true)`
///                                  <td> Visualize the uncertainty on the subset of parameters 'param', as given in fitres, at 'Z' sigma'
/// </table>
///
/// Details on error band visualization
/// -----------------------------------
/// *VisualizeError() uses plotOnWithErrorBand(). Documentation of the latter:*
/// \see plotOnWithErrorBand()

RooPlot* RooAbsReal::plotOn(RooPlot* frame, const RooCmdArg& arg1, const RooCmdArg& arg2,
             const RooCmdArg& arg3, const RooCmdArg& arg4,
             const RooCmdArg& arg5, const RooCmdArg& arg6,
             const RooCmdArg& arg7, const RooCmdArg& arg8,
             const RooCmdArg& arg9, const RooCmdArg& arg10) const
{
  CREATE_CMD_LIST;
  l.Add((TObject*)&arg9) ;  l.Add((TObject*)&arg10) ;
  return plotOn(frame,l) ;
}



////////////////////////////////////////////////////////////////////////////////
/// Internal back-end function of plotOn() with named arguments

RooPlot* RooAbsReal::plotOn(RooPlot* frame, RooLinkedList& argList) const
{
  // Special handling here if argList contains RangeWithName argument with multiple
  // range names -- Need to translate this call into multiple calls

  RooCmdArg* rcmd = static_cast<RooCmdArg*>(argList.FindObject("RangeWithName")) ;
  if (rcmd && TString(rcmd->getString(0)).Contains(",")) {

    // List joint ranges as choice of normalization for all later processing
    RooCmdArg rnorm = RooFit::NormRange(rcmd->getString(0)) ;
    argList.Add(&rnorm) ;

    for (const auto& rangeString : ROOT::Split(rcmd->getString(0), ",")) {
      // Process each range with a separate command with a single range to be plotted
      rcmd->setString(0, rangeString.c_str());
      RooAbsReal::plotOn(frame,argList);
    }
    return frame ;

  }

  // Define configuration for this method
  RooCmdConfig pc("RooAbsReal::plotOn(" + std::string(GetName()) + ")");
  pc.defineString("drawOption","DrawOption",0,"L") ;
  pc.defineString("projectionRangeName","ProjectionRange",0,"",true) ;
  pc.defineString("curveNameSuffix","CurveNameSuffix",0,"") ;
  pc.defineString("sliceCatState","SliceCat",0,"",true) ;
  pc.defineDouble("scaleFactor","Normalization",0,1.0) ;
  pc.defineInt("scaleType","Normalization",0,Relative) ;
  pc.defineSet("sliceSet","SliceVars",0) ;
  pc.defineObject("sliceCatList","SliceCat",0,nullptr,true) ;
  // This dummy is needed for plotOn to recognize the "SliceCatMany" command.
  // It is not used directly, but the "SliceCat" commands are nested in it.
  // Removing this dummy definition results in "ERROR: unrecognized command: SliceCatMany".
  pc.defineObject("dummy1","SliceCatMany",0) ;
  pc.defineSet("projSet","Project",0) ;
  pc.defineObject("asymCat","Asymmetry",0) ;
  pc.defineDouble("precision","Precision",0,1e-3) ;
  pc.defineDouble("evalErrorVal","EvalErrorValue",0,0) ;
  pc.defineInt("doEvalError","EvalErrorValue",0,0) ;
  pc.defineInt("shiftToZero","ShiftToZero",0,0) ;
  pc.defineSet("projDataSet","ProjData",0) ;
  pc.defineObject("projData","ProjData",1) ;
  pc.defineObject("errorFR","VisualizeError",0) ;
  pc.defineDouble("errorZ","VisualizeError",0,1.) ;
  pc.defineSet("errorPars","VisualizeError",0) ;
  pc.defineInt("linearMethod","VisualizeError",0,0) ;
  pc.defineInt("binProjData","ProjData",0,0) ;
  pc.defineDouble("rangeLo","Range",0,-999.) ;
  pc.defineDouble("rangeHi","Range",1,-999.) ;
  pc.defineInt("numee","PrintEvalErrors",0,10) ;
  pc.defineInt("rangeAdjustNorm","Range",0,0) ;
  pc.defineInt("rangeWNAdjustNorm","RangeWithName",0,0) ;
  pc.defineInt("VLines","VLines",0,2) ; // 2==ExtendedWings
  pc.defineString("rangeName","RangeWithName",0,"") ;
  pc.defineString("normRangeName","NormRange",0,"") ;
  pc.defineInt("markerColor","MarkerColor",0,-999) ;
  pc.defineInt("markerStyle","MarkerStyle",0,-999) ;
  pc.defineDouble("markerSize","MarkerSize",0,-999) ;
  pc.defineInt("lineColor","LineColor",0,-999) ;
  pc.defineInt("lineStyle","LineStyle",0,-999) ;
  pc.defineInt("lineWidth","LineWidth",0,-999) ;
  pc.defineInt("fillColor","FillColor",0,-999) ;
  pc.defineInt("fillStyle","FillStyle",0,-999) ;
  pc.defineString("curveName","Name",0,"") ;
  pc.defineInt("curveInvisible","Invisible",0,0) ;
  pc.defineInt("showProg","ShowProgress",0,0) ;
  pc.defineInt("numCPU","NumCPU",0,1) ;
  pc.defineInt("interleave","NumCPU",1,0) ;
  pc.defineString("addToCurveName","AddTo",0,"") ;
  pc.defineDouble("addToWgtSelf","AddTo",0,1.) ;
  pc.defineDouble("addToWgtOther","AddTo",1,1.) ;
  pc.defineInt("moveToBack","MoveToBack",0,0) ;
  pc.defineMutex("SliceVars","Project") ;
  pc.defineMutex("AddTo","Asymmetry") ;
  pc.defineMutex("Range","RangeWithName") ;
  pc.defineMutex("VisualizeError","VisualizeErrorData") ;

  // Process & check varargs
  pc.process(argList) ;
  if (!pc.ok(true)) {
    return frame ;
  }

  TString drawOpt(pc.getString("drawOption"));

  RooFitResult* errFR = static_cast<RooFitResult*>(pc.getObject("errorFR")) ;
  if (!drawOpt.Contains("P") && errFR) {
      return plotOnWithErrorBand(frame, *errFR, pc.getDouble("errorZ"), pc.getSet("errorPars"), argList,
                                 pc.getInt("linearMethod"));
  }

  // Extract values from named arguments
  PlotOpt o ;
  o.numee       = pc.getInt("numee") ;
  o.drawOptions = drawOpt.Data();
  o.curveNameSuffix = pc.getString("curveNameSuffix") ;
  o.scaleFactor = pc.getDouble("scaleFactor") ;
  o.stype = (ScaleType) pc.getInt("scaleType")  ;
  o.projData = static_cast<const RooAbsData*>(pc.getObject("projData")) ;
  o.binProjData = pc.getInt("binProjData") ;
  o.projDataSet = pc.getSet("projDataSet");
  o.numCPU = pc.getInt("numCPU") ;
  o.interleave = (RooFit::MPSplit) pc.getInt("interleave") ;
  o.eeval      = pc.getDouble("evalErrorVal") ;
  o.doeeval   = pc.getInt("doEvalError") ;
  o.errorFR = errFR;

  const RooArgSet* sliceSetTmp = pc.getSet("sliceSet");
  std::unique_ptr<RooArgSet> sliceSet{sliceSetTmp ? static_cast<RooArgSet*>(sliceSetTmp->Clone()) : nullptr};
  const RooArgSet* projSet = pc.getSet("projSet") ;
  const RooAbsCategoryLValue* asymCat = static_cast<const RooAbsCategoryLValue*>(pc.getObject("asymCat")) ;


  // Look for category slice arguments and add them to the master slice list if found
  if (const char* sliceCatState = pc.getString("sliceCatState",nullptr,true)) {
    const RooLinkedList& sliceCatList = pc.getObjectList("sliceCatList") ;

    // Make the master slice set if it doesnt exist
    if (!sliceSet) {
      sliceSet = std::make_unique<RooArgSet>();
    }

    // Loop over all categories provided by (multiple) Slice() arguments
    auto iter = sliceCatList.begin();
    for (auto const& catToken : ROOT::Split(sliceCatState, ",")) {
      if (auto scat = static_cast<RooCategory*>(*iter)) {
        // Set the slice position to the value indicate by slabel
        scat->setLabel(catToken);
        // Add the slice category to the master slice set
        sliceSet->add(*scat,false) ;
      }
      ++iter;
    }
  }

  o.precision = pc.getDouble("precision") ;
  o.shiftToZero = (pc.getInt("shiftToZero")!=0) ;
  Int_t vlines = pc.getInt("VLines");
  if (pc.hasProcessed("Range")) {
    o.rangeLo = pc.getDouble("rangeLo") ;
    o.rangeHi = pc.getDouble("rangeHi") ;
    o.postRangeFracScale = pc.getInt("rangeAdjustNorm") ;
    if (vlines==2) vlines=0 ; // Default is NoWings if range was specified
  } else if (pc.hasProcessed("RangeWithName")) {
    o.normRangeName = pc.getString("rangeName",nullptr,true) ;
    o.rangeLo = frame->getPlotVar()->getMin(pc.getString("rangeName",nullptr,true)) ;
    o.rangeHi = frame->getPlotVar()->getMax(pc.getString("rangeName",nullptr,true)) ;
    o.postRangeFracScale = pc.getInt("rangeWNAdjustNorm") ;
    if (vlines==2) vlines=0 ; // Default is NoWings if range was specified
  }


  // If separate normalization range was specified this overrides previous settings
  if (pc.hasProcessed("NormRange")) {
    o.normRangeName = pc.getString("normRangeName") ;
    o.postRangeFracScale = true ;
  }

  o.wmode = (vlines==2)?RooCurve::Extended:(vlines==1?RooCurve::Straight:RooCurve::NoWings) ;
  o.projectionRangeName = pc.getString("projectionRangeName",nullptr,true) ;
  o.curveName = pc.getString("curveName",nullptr,true) ;
  o.curveInvisible = pc.getInt("curveInvisible") ;
  o.progress = pc.getInt("showProg") ;
  o.addToCurveName = pc.getString("addToCurveName",nullptr,true) ;
  o.addToWgtSelf = pc.getDouble("addToWgtSelf") ;
  o.addToWgtOther = pc.getDouble("addToWgtOther") ;

  if (o.addToCurveName && !frame->findObject(o.addToCurveName,RooCurve::Class())) {
    coutE(InputArguments) << "RooAbsReal::plotOn(" << GetName() << ") cannot find existing curve " << o.addToCurveName << " to add to in RooPlot" << std::endl ;
    return frame ;
  }

  RooArgSet projectedVars ;
  if (sliceSet) {
    cxcoutD(Plotting) << "RooAbsReal::plotOn(" << GetName() << ") Preprocessing: have slice " << *sliceSet << std::endl ;

    makeProjectionSet(frame->getPlotVar(),frame->getNormVars(),projectedVars,true) ;

    // Take out the sliced variables
    for (const auto sliceArg : *sliceSet) {
      if (RooAbsArg* arg = projectedVars.find(sliceArg->GetName())) {
        projectedVars.remove(*arg) ;
      } else {
        coutI(Plotting) << "RooAbsReal::plotOn(" << GetName() << ") slice variable "
            << sliceArg->GetName() << " was not projected anyway" << std::endl ;
      }
    }
  } else if (projSet) {
    cxcoutD(Plotting) << "RooAbsReal::plotOn(" << GetName() << ") Preprocessing: have projSet " << *projSet << std::endl ;
    makeProjectionSet(frame->getPlotVar(),projSet,projectedVars,false) ;
  } else {
    cxcoutD(Plotting) << "RooAbsReal::plotOn(" << GetName() << ") Preprocessing: have neither sliceSet nor projSet " << std::endl ;
    makeProjectionSet(frame->getPlotVar(),frame->getNormVars(),projectedVars,true) ;
  }
  o.projSet = &projectedVars ;

  cxcoutD(Plotting) << "RooAbsReal::plotOn(" << GetName() << ") Preprocessing: projectedVars = " << projectedVars << std::endl ;


  // Forward to actual calculation
  RooPlot* ret = asymCat ? RooAbsReal::plotAsymOn(frame,*asymCat,o) : RooAbsReal::plotOn(frame,o);

  // Optionally adjust line/fill attributes
  Int_t lineColor = pc.getInt("lineColor") ;
  Int_t lineStyle = pc.getInt("lineStyle") ;
  Int_t lineWidth = pc.getInt("lineWidth") ;
  Int_t markerColor = pc.getInt("markerColor") ;
  Int_t markerStyle = pc.getInt("markerStyle") ;
  Size_t markerSize  = pc.getDouble("markerSize") ;
  Int_t fillColor = pc.getInt("fillColor") ;
  Int_t fillStyle = pc.getInt("fillStyle") ;
  if (lineColor!=-999) ret->getAttLine()->SetLineColor(lineColor) ;
  if (lineStyle!=-999) ret->getAttLine()->SetLineStyle(lineStyle) ;
  if (lineWidth!=-999) ret->getAttLine()->SetLineWidth(lineWidth) ;
  if (fillColor!=-999) ret->getAttFill()->SetFillColor(fillColor) ;
  if (fillStyle!=-999) ret->getAttFill()->SetFillStyle(fillStyle) ;
  if (markerColor!=-999) ret->getAttMarker()->SetMarkerColor(markerColor) ;
  if (markerStyle!=-999) ret->getAttMarker()->SetMarkerStyle(markerStyle) ;
  if (markerSize!=-999) ret->getAttMarker()->SetMarkerSize(markerSize) ;

  if ((fillColor != -999 || fillStyle != -999) && !drawOpt.Contains("F")) {
    coutW(Plotting) << "Fill color or style was set for plotting \"" << GetName()
        << "\", but these only have an effect when 'DrawOption(\"F\")' for fill is used at the same time." << std::endl;
  }

  // Move last inserted object to back to drawing stack if requested
  if (pc.getInt("moveToBack") && frame->numItems()>1) {
    frame->drawBefore(frame->getObject(0)->GetName(), frame->getCurve()->GetName());
  }

  return ret ;
}



/// Plotting engine function for internal use
///
/// Plot ourselves on given frame. If frame contains a histogram, all dimensions of the plotted
/// function that occur in the previously plotted dataset are projected via partial integration,
/// otherwise no projections are performed. Optionally, certain projections can be performed
/// by summing over the values present in a provided dataset ('projData'), to correctly
/// project out data dependents that are not properly described by the PDF (e.g. per-event errors).
///
/// The functions value can be multiplied with an optional scale factor. The interpretation
/// of the scale factor is unique for generic real functions, for PDFs there are various interpretations
/// possible, which can be selection with 'stype' (see RooAbsPdf::plotOn() for details).
///
/// The default projection behaviour can be overridden by supplying an optional set of dependents
/// to project via RooFit command arguments.
//_____________________________________________________________________________
// coverity[PASS_BY_VALUE]
RooPlot* RooAbsReal::plotOn(RooPlot *frame, PlotOpt o) const
{
  // Sanity checks
  if (plotSanityChecks(frame)) return frame ;

  // ProjDataVars is either all projData observables, or the user indicated subset of it
  RooArgSet projDataVars ;
  if (o.projData) {
    cxcoutD(Plotting) << "RooAbsReal::plotOn(" << GetName() << ") have ProjData with observables = " << *o.projData->get() << std::endl ;
    if (o.projDataSet) {
      projDataVars.add(*std::unique_ptr<RooArgSet>{o.projData->get()->selectCommon(*o.projDataSet)}) ;
      cxcoutD(Plotting) << "RooAbsReal::plotOn(" << GetName() << ") have ProjDataSet = " << *o.projDataSet << " will only use this subset of projData" << std::endl ;
    } else {
      cxcoutD(Plotting) << "RooAbsReal::plotOn(" << GetName() << ") using full ProjData" << std::endl ;
      projDataVars.add(*o.projData->get()) ;
    }
  }

  cxcoutD(Plotting) << "RooAbsReal::plotOn(" << GetName() << ") ProjDataVars = " << projDataVars << std::endl ;

  // Make list of variables to be projected
  RooArgSet projectedVars ;
  RooArgSet sliceSet ;
  if (o.projSet) {
    cxcoutD(Plotting) << "RooAbsReal::plotOn(" << GetName() << ") have input projSet = " << *o.projSet << std::endl ;
    makeProjectionSet(frame->getPlotVar(),o.projSet,projectedVars,false) ;
    cxcoutD(Plotting) << "RooAbsReal::plotOn(" << GetName() << ") calculated projectedVars = " << *o.projSet << std::endl ;

    // Print list of non-projected variables
    if (frame->getNormVars()) {
      RooArgSet sliceSetTmp;
      getObservables(frame->getNormVars(), sliceSetTmp) ;

      cxcoutD(Plotting) << "RooAbsReal::plotOn(" << GetName() << ") frame->getNormVars() that are also observables = " << sliceSetTmp << std::endl ;

      sliceSetTmp.remove(projectedVars,true,true) ;
      sliceSetTmp.remove(*frame->getPlotVar(),true,true) ;

      if (o.projData) {
        std::unique_ptr<RooArgSet> tmp{projDataVars.selectCommon(*o.projSet)};
        sliceSetTmp.remove(*tmp,true,true) ;
      }

      if (!sliceSetTmp.empty()) {
   coutI(Plotting) << "RooAbsReal::plotOn(" << GetName() << ") plot on "
         << frame->getPlotVar()->GetName() << " represents a slice in " << sliceSetTmp << std::endl ;
      }
      sliceSet.add(sliceSetTmp) ;
    }
  } else {
    makeProjectionSet(frame->getPlotVar(),frame->getNormVars(),projectedVars,true) ;
  }

  cxcoutD(Plotting) << "RooAbsReal::plotOn(" << GetName() << ") projectedVars = " << projectedVars << " sliceSet = " << sliceSet << std::endl ;


  RooArgSet* projDataNeededVars = nullptr ;
  // Take out data-projected dependents from projectedVars
  if (o.projData) {
    projDataNeededVars = projectedVars.selectCommon(projDataVars);
    projectedVars.remove(projDataVars,true,true) ;
  }

  // Get the plot variable and remember its original value
  auto* plotVar = static_cast<RooRealVar*>(frame->getPlotVar());
  double oldPlotVarVal = plotVar->getVal();

  // Inform user about projections
  if (!projectedVars.empty()) {
    coutI(Plotting) << "RooAbsReal::plotOn(" << GetName() << ") plot on " << plotVar->GetName()
          << " integrates over variables " << projectedVars
          << (o.projectionRangeName?Form(" in range %s",o.projectionRangeName):"") << std::endl;
  }
  if (projDataNeededVars && !projDataNeededVars->empty()) {
    coutI(Plotting) << "RooAbsReal::plotOn(" << GetName() << ") plot on " << plotVar->GetName()
          << " averages using data variables " << *projDataNeededVars << std::endl ;
  }

  // Create projection integral
  RooArgSet* projectionCompList = nullptr ;

  RooArgSet deps;
  getObservables(frame->getNormVars(), deps) ;
  deps.remove(projectedVars,true,true) ;
  if (projDataNeededVars) {
    deps.remove(*projDataNeededVars,true,true) ;
  }
  deps.remove(*plotVar,true,true) ;
  deps.add(*plotVar) ;

  // Now that we have the final set of dependents, call checkObservables()

  // WVE take out conditional observables
  if (checkObservables(&deps)) {
    coutE(Plotting) << "RooAbsReal::plotOn(" << GetName() << ") error in checkObservables, abort" << std::endl ;
    if (projDataNeededVars) delete projDataNeededVars ;
    return frame ;
  }

  RooAbsReal *projection = const_cast<RooAbsReal*>(createPlotProjection(deps, &projectedVars, projectionCompList, o.projectionRangeName));
  cxcoutD(Plotting) << "RooAbsReal::plotOn(" << GetName() << ") plot projection object is " << projection->GetName() << std::endl ;
  if (dologD(Plotting)) {
    projection->printStream(ccoutD(Plotting),0,kVerbose) ;
  }

  // Always fix RooAddPdf normalizations
  RooArgSet fullNormSet(deps) ;
  fullNormSet.add(projectedVars) ;
  if (projDataNeededVars && !projDataNeededVars->empty()) {
    fullNormSet.add(*projDataNeededVars) ;
  }

  std::unique_ptr<RooArgSet> projectionComponents(projection->getComponents());
  for(auto * pdf : dynamic_range_cast<RooAbsPdf*>(*projectionComponents)) {
    if (pdf) {
      pdf->selectNormalization(&fullNormSet) ;
    }
  }

  // Apply data projection, if requested
  if (o.projData && projDataNeededVars && !projDataNeededVars->empty()) {

    // If data set contains more rows than needed, make reduced copy first
    RooAbsData* projDataSel = const_cast<RooAbsData*>(o.projData);
    std::unique_ptr<RooAbsData> projDataSelOwned;

    if (projDataNeededVars->size() < o.projData->get()->size()) {

      // Determine if there are any slice variables in the projection set
      RooArgSet sliceDataSet;
      sliceSet.selectCommon(*o.projData->get(), sliceDataSet);
      std::string cutString = RooFit::Detail::makeSliceCutString(sliceDataSet);

      if (!cutString.empty()) {
       coutI(Plotting) << "RooAbsReal::plotOn(" << GetName() << ") reducing given projection dataset to entries with " << cutString << std::endl ;
      }
      projDataSelOwned = std::unique_ptr<RooAbsData>{projDataSel->reduce(RooFit::SelectVars(*projDataNeededVars), RooFit::Cut(cutString.c_str()))};
      projDataSel = projDataSelOwned.get();
      coutI(Plotting) << "RooAbsReal::plotOn(" << GetName()
            << ") only the following components of the projection data will be used: " << *projDataNeededVars << std::endl ;
    }

    // Request binning of unbinned projection dataset that consists exclusively of category observables
    if (!o.binProjData && dynamic_cast<RooDataSet*>(projDataSel)!=nullptr) {

      // Determine if dataset contains only categories
      bool allCat(true) ;
      for(RooAbsArg * arg2 : *projDataSel->get()) {
   if (!dynamic_cast<RooCategory*>(arg2)) allCat = false ;
      }
      if (allCat) {
   o.binProjData = true ;
   coutI(Plotting) << "RooAbsReal::plotOn(" << GetName() << ") unbinned projection dataset consist only of discrete variables,"
         << " performing projection with binned copy for optimization." << std::endl ;

      }
    }

    // Bin projection dataset if requested
    if (o.binProjData) {
      projDataSelOwned = std::make_unique<RooDataHist>(std::string(projDataSel->GetName()) + "_binned","Binned projection data",*projDataSel->get(),*projDataSel);
      projDataSel = projDataSelOwned.get();
    }

    // Construct scaled data weighted average
    ScaledDataWeightedAverage scaleBind{*projection, *projDataSel, o.scaleFactor, *plotVar};

    // Set default range, if not specified
    if (o.rangeLo==0 && o.rangeHi==0) {
      o.rangeLo = frame->GetXaxis()->GetXmin() ;
      o.rangeHi = frame->GetXaxis()->GetXmax() ;
    }

    // Construct name of curve for data weighed average
    std::string curveName(projection->GetName()) ;
    curveName.append("_DataAvg[" + projDataSel->get()->contentsString() + "]");
    // Append slice set specification if any
    if (!sliceSet.empty()) {
      curveName.append("_Slice[" + sliceSet.contentsString() + "]");
    }
    // Append any suffixes imported from RooAbsPdf::plotOn
    if (o.curveNameSuffix) {
      curveName.append(o.curveNameSuffix) ;
    }

    // Curve constructor for data weighted average
    RooAbsReal::setEvalErrorLoggingMode(RooAbsReal::CollectErrors) ;
    RooCurve *curve = new RooCurve(projection->GetName(),projection->GetTitle(),scaleBind,
               o.rangeLo,o.rangeHi,frame->GetNbinsX(),o.precision,o.precision,o.shiftToZero,o.wmode,o.numee,o.doeeval,o.eeval) ;
    RooAbsReal::setEvalErrorLoggingMode(RooAbsReal::PrintErrors) ;

    curve->SetName(curveName.c_str()) ;

    // Add self to other curve if requested
    if (o.addToCurveName) {
      RooCurve* otherCurve = static_cast<RooCurve*>(frame->findObject(o.addToCurveName,RooCurve::Class())) ;

      // Curve constructor for sum of curves
      RooCurve* sumCurve = new RooCurve(projection->GetName(),projection->GetTitle(),*curve,*otherCurve,o.addToWgtSelf,o.addToWgtOther) ;
      sumCurve->SetName(Form("%s_PLUS_%s",curve->GetName(),otherCurve->GetName())) ;
      delete curve ;
      curve = sumCurve ;

    }

    if (o.curveName) {
      curve->SetName(o.curveName) ;
    }

    // add this new curve to the specified plot frame
    frame->addPlotable(curve, o.drawOptions, o.curveInvisible);

  } else {

    // Set default range, if not specified
    if (o.rangeLo==0 && o.rangeHi==0) {
      o.rangeLo = frame->GetXaxis()->GetXmin() ;
      o.rangeHi = frame->GetXaxis()->GetXmax() ;
    }

    // Calculate a posteriori range fraction scaling if requested (2nd part of normalization correction for
    // result fit on subrange of data)
    if (o.postRangeFracScale) {
      if (!o.normRangeName) {
   o.normRangeName = "plotRange" ;
   plotVar->setRange("plotRange",o.rangeLo,o.rangeHi) ;
      }

      // Evaluate fractional correction integral always on full p.d.f, not component.
      GlobalSelectComponentRAII selectCompRAII(true);
      std::unique_ptr<RooAbsReal> intFrac{projection->createIntegral(*plotVar,*plotVar,o.normRangeName)};
      if(o.stype != RooAbsReal::Raw || this->InheritsFrom(RooAbsPdf::Class())){
        // this scaling should only be !=1  when plotting partial ranges
        // still, raw means raw
        o.scaleFactor /= intFrac->getVal() ;
      }
    }

    // create a new curve of our function using the clone to do the evaluations
    // Curve constructor for regular projections

    // Set default name of curve
    std::string curveName(projection->GetName()) ;
    if (!sliceSet.empty()) {
      curveName.append("_Slice[" + sliceSet.contentsString() + "]");
    }
    if (o.curveNameSuffix) {
      // Append any suffixes imported from RooAbsPdf::plotOn
      curveName.append(o.curveNameSuffix) ;
    }

    TString opt(o.drawOptions);
    if(opt.Contains("P")){
      RooAbsReal::setEvalErrorLoggingMode(RooAbsReal::CollectErrors) ;
      RooHist *graph= new RooHist(*projection,*plotVar,1.,o.scaleFactor,frame->getNormVars(),o.errorFR);
      RooAbsReal::setEvalErrorLoggingMode(RooAbsReal::PrintErrors) ;

      // Override name of curve by user name, if specified
      if (o.curveName) {
        graph->SetName(o.curveName) ;
      }

      // add this new curve to the specified plot frame
      frame->addPlotable(graph, o.drawOptions, o.curveInvisible);
    } else {
      RooAbsReal::setEvalErrorLoggingMode(RooAbsReal::CollectErrors) ;
      RooCurve *curve = new RooCurve(*projection,*plotVar,o.rangeLo,o.rangeHi,frame->GetNbinsX(),
                                     o.scaleFactor,nullptr,o.precision,o.precision,o.shiftToZero,o.wmode,o.numee,o.doeeval,o.eeval,o.progress);
      RooAbsReal::setEvalErrorLoggingMode(RooAbsReal::PrintErrors) ;
      curve->SetName(curveName.c_str()) ;

      // Add self to other curve if requested
      if (o.addToCurveName) {
        RooCurve* otherCurve = static_cast<RooCurve*>(frame->findObject(o.addToCurveName,RooCurve::Class())) ;
        RooCurve* sumCurve = new RooCurve(projection->GetName(),projection->GetTitle(),*curve,*otherCurve,o.addToWgtSelf,o.addToWgtOther) ;
        sumCurve->SetName(Form("%s_PLUS_%s",curve->GetName(),otherCurve->GetName())) ;
        delete curve ;
        curve = sumCurve ;
      }

      // Override name of curve by user name, if specified
      if (o.curveName) {
        curve->SetName(o.curveName) ;
      }

      // add this new curve to the specified plot frame
      frame->addPlotable(curve, o.drawOptions, o.curveInvisible);
    }
  }

  if (projDataNeededVars) delete projDataNeededVars ;
  delete projectionCompList ;
  plotVar->setVal(oldPlotVarVal); // reset the plot variable value to not disturb the original state
  return frame;
}


//_____________________________________________________________________________
// coverity[PASS_BY_VALUE]
RooPlot* RooAbsReal::plotAsymOn(RooPlot *frame, const RooAbsCategoryLValue& asymCat, PlotOpt o) const

{
  // Plotting engine for asymmetries. Implements the functionality if plotOn(frame,Asymmetry(...)))
  //
  // Plot asymmetry of ourselves, defined as
  //
  //   asym = f(asymCat=-1) - f(asymCat=+1) / ( f(asymCat=-1) + f(asymCat=+1) )
  //
  // on frame. If frame contains a histogram, all dimensions of the plotted
  // asymmetry function that occur in the previously plotted dataset are projected via partial integration.
  // Otherwise no projections are performed,
  //
  // The asymmetry function can be multiplied with an optional scale factor. The default projection
  // behaviour can be overridden by supplying an optional set of dependents to project.

  // Sanity checks
  if (plotSanityChecks(frame)) return frame ;

  // ProjDataVars is either all projData observables, or the user indicated subset of it
  RooArgSet projDataVars ;
  if (o.projData) {
    if (o.projDataSet) {
      std::unique_ptr<RooArgSet> tmp{o.projData->get()->selectCommon(*o.projDataSet)};
      projDataVars.add(*tmp) ;
    } else {
      projDataVars.add(*o.projData->get()) ;
    }
  }

  // Must depend on asymCat
  if (!dependsOn(asymCat)) {
    coutE(Plotting) << "RooAbsReal::plotAsymOn(" << GetName()
          << ") function doesn't depend on asymmetry category " << asymCat.GetName() << std::endl ;
    return frame ;
  }

  // asymCat must be a signCat
  if (!asymCat.isSignType()) {
    coutE(Plotting) << "RooAbsReal::plotAsymOn(" << GetName()
          << ") asymmetry category must have 2 or 3 states with index values -1,0,1" << std::endl ;
    return frame ;
  }

  // Make list of variables to be projected
  RooArgSet projectedVars ;
  RooArgSet sliceSet ;
  if (o.projSet) {
    makeProjectionSet(frame->getPlotVar(),o.projSet,projectedVars,false) ;

    // Print list of non-projected variables
    if (frame->getNormVars()) {
      RooArgSet sliceSetTmp;
      getObservables(frame->getNormVars(), sliceSetTmp) ;
      sliceSetTmp.remove(projectedVars,true,true) ;
      sliceSetTmp.remove(*frame->getPlotVar(),true,true) ;

      if (o.projData) {
        std::unique_ptr<RooArgSet> tmp{projDataVars.selectCommon(*o.projSet)};
        sliceSetTmp.remove(*tmp,true,true) ;
      }

      if (!sliceSetTmp.empty()) {
   coutI(Plotting) << "RooAbsReal::plotAsymOn(" << GetName() << ") plot on "
         << frame->getPlotVar()->GetName() << " represents a slice in " << sliceSetTmp << std::endl ;
      }
      sliceSet.add(sliceSetTmp) ;
    }
  } else {
    makeProjectionSet(frame->getPlotVar(),frame->getNormVars(),projectedVars,true) ;
  }


  // Take out data-projected dependents from projectedVars
  RooArgSet* projDataNeededVars = nullptr ;
  if (o.projData) {
    projDataNeededVars = projectedVars.selectCommon(projDataVars);
    projectedVars.remove(projDataVars,true,true) ;
  }

  // Take out plotted asymmetry from projection
  if (projectedVars.find(asymCat.GetName())) {
    projectedVars.remove(*projectedVars.find(asymCat.GetName())) ;
  }

  // Clone the plot variable
  RooAbsReal* realVar = static_cast<RooRealVar*>(frame->getPlotVar()) ;
  RooRealVar* plotVar = static_cast<RooRealVar*>(realVar->Clone()) ;

  // Inform user about projections
  if (!projectedVars.empty()) {
    coutI(Plotting) << "RooAbsReal::plotAsymOn(" << GetName() << ") plot on " << plotVar->GetName()
          << " projects variables " << projectedVars << std::endl ;
  }
  if (projDataNeededVars && !projDataNeededVars->empty()) {
    coutI(Plotting) << "RooAbsReal::plotOn(" << GetName() << ") plot on " << plotVar->GetName()
          << " averages using data variables "<<  *projDataNeededVars << std::endl ;
  }


  // Customize two copies of projection with fixed negative and positive asymmetry
  std::unique_ptr<RooAbsCategoryLValue> asymPos{static_cast<RooAbsCategoryLValue*>(asymCat.Clone("asym_pos"))};
  std::unique_ptr<RooAbsCategoryLValue> asymNeg{static_cast<RooAbsCategoryLValue*>(asymCat.Clone("asym_neg"))};
  asymPos->setIndex(1) ;
  asymNeg->setIndex(-1) ;
  RooCustomizer custPos{*this,"pos"};
  RooCustomizer custNeg{*this,"neg"};
  //custPos->setOwning(true) ;
  //custNeg->setOwning(true) ;
  custPos.replaceArg(asymCat,*asymPos) ;
  custNeg.replaceArg(asymCat,*asymNeg) ;
  std::unique_ptr<RooAbsReal> funcPos{static_cast<RooAbsReal*>(custPos.build())};
  std::unique_ptr<RooAbsReal> funcNeg{static_cast<RooAbsReal*>(custNeg.build())};

  // Create projection integral
  RooArgSet *posProjCompList;
  RooArgSet *negProjCompList;

  // Add projDataVars to normalized dependents of projection
  // This is needed only for asymmetries (why?)
  RooArgSet depPos(*plotVar,*asymPos) ;
  RooArgSet depNeg(*plotVar,*asymNeg) ;
  depPos.add(projDataVars) ;
  depNeg.add(projDataVars) ;

  const RooAbsReal *posProj = funcPos->createPlotProjection(depPos, &projectedVars, posProjCompList, o.projectionRangeName) ;
  const RooAbsReal *negProj = funcNeg->createPlotProjection(depNeg, &projectedVars, negProjCompList, o.projectionRangeName) ;
  if (!posProj || !negProj) {
    coutE(Plotting) << "RooAbsReal::plotAsymOn(" << GetName() << ") Unable to create projections, abort" << std::endl ;
    return frame ;
  }

  // Create a RooFormulaVar representing the asymmetry
  TString asymName(GetName()) ;
  asymName.Append("_Asym[") ;
  asymName.Append(asymCat.GetName()) ;
  asymName.Append("]") ;
  TString asymTitle(asymCat.GetName()) ;
  asymTitle.Append(" Asymmetry of ") ;
  asymTitle.Append(GetTitle()) ;
  RooFormulaVar funcAsym{asymName,asymTitle,"(@0-@1)/(@0+@1)",RooArgSet(*posProj,*negProj)};

  if (o.projData) {

    // If data set contains more rows than needed, make reduced copy first
    RooAbsData* projDataSel = const_cast<RooAbsData*>(o.projData);
    std::unique_ptr<RooAbsData> projDataSelOwned;
    if (projDataNeededVars && projDataNeededVars->size() < o.projData->get()->size()) {

      // Determine if there are any slice variables in the projection set
      RooArgSet sliceDataSet;
      sliceSet.selectCommon(*o.projData->get(), sliceDataSet);
      std::string cutString = RooFit::Detail::makeSliceCutString(sliceDataSet);

      if (!cutString.empty()) {
   coutI(Plotting) << "RooAbsReal::plotAsymOn(" << GetName()
         << ") reducing given projection dataset to entries with " << cutString << std::endl ;
      }
      projDataSelOwned = std::unique_ptr<RooAbsData>{projDataSel->reduce(RooFit::SelectVars(*projDataNeededVars),RooFit::Cut(cutString.c_str()))};
      projDataSel = projDataSelOwned.get();
      coutI(Plotting) << "RooAbsReal::plotAsymOn(" << GetName()
            << ") only the following components of the projection data will be used: " << *projDataNeededVars << std::endl ;
    }


    // Construct scaled data weighted average
    ScaledDataWeightedAverage scaleBind{funcAsym, *projDataSel, o.scaleFactor, *plotVar};

    // Set default range, if not specified
    if (o.rangeLo==0 && o.rangeHi==0) {
      o.rangeLo = frame->GetXaxis()->GetXmin() ;
      o.rangeHi = frame->GetXaxis()->GetXmax() ;
    }

    // Construct name of curve for data weighed average
    TString curveName(funcAsym.GetName()) ;
    curveName.Append(Form("_DataAvg[%s]",projDataSel->get()->contentsString().c_str())) ;
    // Append slice set specification if any
    if (!sliceSet.empty()) {
      curveName.Append(Form("_Slice[%s]",sliceSet.contentsString().c_str())) ;
    }
    // Append any suffixes imported from RooAbsPdf::plotOn
    if (o.curveNameSuffix) {
      curveName.Append(o.curveNameSuffix) ;
    }


    RooAbsReal::setEvalErrorLoggingMode(RooAbsReal::CollectErrors) ;
    RooCurve *curve = new RooCurve(funcAsym.GetName(),funcAsym.GetTitle(),scaleBind,
               o.rangeLo,o.rangeHi,frame->GetNbinsX(),o.precision,o.precision,false,o.wmode,o.numee,o.doeeval,o.eeval) ;
    RooAbsReal::setEvalErrorLoggingMode(RooAbsReal::PrintErrors) ;

    dynamic_cast<TAttLine*>(curve)->SetLineColor(2) ;
    // add this new curve to the specified plot frame
    frame->addPlotable(curve, o.drawOptions);

    ccoutW(Eval) << std::endl ;
  } else {

    // Set default range, if not specified
    if (o.rangeLo==0 && o.rangeHi==0) {
      o.rangeLo = frame->GetXaxis()->GetXmin() ;
      o.rangeHi = frame->GetXaxis()->GetXmax() ;
    }

    RooAbsReal::setEvalErrorLoggingMode(RooAbsReal::CollectErrors) ;
    RooCurve* curve= new RooCurve(funcAsym,*plotVar,o.rangeLo,o.rangeHi,frame->GetNbinsX(),
              o.scaleFactor,nullptr,o.precision,o.precision,false,o.wmode,o.numee,o.doeeval,o.eeval);
    RooAbsReal::setEvalErrorLoggingMode(RooAbsReal::PrintErrors) ;

    dynamic_cast<TAttLine*>(curve)->SetLineColor(2) ;


    // Set default name of curve
    TString curveName(funcAsym.GetName()) ;
    if (!sliceSet.empty()) {
      curveName.Append(Form("_Slice[%s]",sliceSet.contentsString().c_str())) ;
    }
    if (o.curveNameSuffix) {
      // Append any suffixes imported from RooAbsPdf::plotOn
      curveName.Append(o.curveNameSuffix) ;
    }
    curve->SetName(curveName.Data()) ;

    // add this new curve to the specified plot frame
    frame->addPlotable(curve, o.drawOptions);

  }

  // Cleanup
  delete posProjCompList ;
  delete negProjCompList ;

  delete plotVar ;

  return frame;
}



////////////////////////////////////////////////////////////////////////////////
/// \brief Propagates parameter uncertainties to an uncertainty estimate for this RooAbsReal.
///
/// Estimates the uncertainty \f$\sigma_f(x;\theta)\f$ on a function \f$f(x;\theta)\f$ represented by this RooAbsReal.
/// Here, \f$\theta\f$ is a vector of parameters with uncertainties \f$\sigma_\theta\f$, and \f$x\f$ are usually observables.
/// The uncertainty is estimated by *linearly* propagating the parameter uncertainties using the correlation matrix from a fit result.
///
/// The square of the uncertainty on \f$f(x;\theta)\f$ is calculated as follows:
/// \f[
///     \sigma_f(x)^2 = \Delta f_i(x) \cdot \mathrm{Corr}_{i, j} \cdot \Delta f_j(x),
/// \f]
/// where \f$ \Delta f_i(x) = \frac{1}{2} \left(f(x;\theta_i + \sigma_{\theta_i}) - f(x; \theta_i - \sigma_{\theta_i}) \right) \f$
/// is the vector of function variations when changing the parameters one at a time, and
/// \f$ \mathrm{Corr}_{i,j} = \left(\sigma_{\theta_i} \sigma_{\theta_j}\right)^{-1} \cdot \mathrm{Cov}_{i,j}  \f$ is the correlation matrix from the fit result.

double RooAbsReal::getPropagatedError(const RooFitResult &fr, const RooArgSet &nset) const
{
  // Calling getParameters() might be costly, but necessary to get the right
  // parameters in the RooAbsReal. The RooFitResult only stores snapshots.
  RooArgSet allParamsInAbsReal;
  getParameters(&nset, allParamsInAbsReal);

  RooArgList paramList;
  for(auto * rrvFitRes : static_range_cast<RooRealVar*>(fr.floatParsFinal())) {

     auto rrvInAbsReal = static_cast<RooRealVar const*>(allParamsInAbsReal.find(*rrvFitRes));

     // If this RooAbsReal is a RooRealVar in the fit result, we don't need to
     // propagate anything and can just return the error in the fit result
     if(rrvFitRes->namePtr() == namePtr()) return rrvFitRes->getError();

     // Strip out parameters with zero error
     if (rrvFitRes->getError() <= std::abs(rrvFitRes->getVal()) * std::numeric_limits<double>::epsilon()) continue;

     // Ignore parameters in the fit result that this RooAbsReal doesn't depend on
     if(!rrvInAbsReal) continue;

     // Checking for float equality is a bad. We check if the values are
     // negligibly far away from each other, relative to the uncertainty.
     if(std::abs(rrvInAbsReal->getVal() - rrvFitRes->getVal()) > 0.01 * rrvFitRes->getError()) {
        std::stringstream errMsg;
        errMsg << "RooAbsReal::getPropagatedError(): the parameters of the RooAbsReal don't have"
               << " the same values as in the fit result! The logic of getPropagatedError is broken in this case.";

        throw std::runtime_error(errMsg.str());
     }

     paramList.add(*rrvInAbsReal);
  }

  std::vector<double> plusVar;
  std::vector<double> minusVar;
  plusVar.reserve(paramList.size());
  minusVar.reserve(paramList.size());

  // Create std::vector of plus,minus variations for each parameter
  TMatrixDSym V(paramList.size() == fr.floatParsFinal().size() ?
      fr.covarianceMatrix() :
      fr.reducedCovarianceMatrix(paramList)) ;

  for (std::size_t ivar=0 ; ivar<paramList.size() ; ivar++) {

    auto& rrv = static_cast<RooRealVar&>(paramList[ivar]);

    double cenVal = rrv.getVal() ;
    double errVal = sqrt(V(ivar,ivar)) ;

    // Check if the variations are still in the parameter range.
    if(!rrv.inRange(cenVal+errVal, nullptr) || !rrv.inRange(cenVal-errVal, nullptr)) {
      std::stringstream ss;
      ss << "RooAbsReal::getPropagatedError(" << GetName() << "): the 1-sigma variations for the parameter "
         << "\"" << rrv.GetName() << "\" are invalid "
         << " because their values (" << cenVal-errVal << ", " << cenVal+errVal
         << ") are outside the defined range [" << rrv.getMin() << ", " << rrv.getMax() << "]!\n"
         << "                         The variations will be clipped inside the range."
         << " This might or might not be acceptable in your usecase.";
      coutW(Plotting) << ss.str() << std::endl;
    }

    // Make Plus variation
    rrv.setVal(std::min(cenVal+errVal, rrv.getMax())) ;
    plusVar.push_back(getVal(nset)) ;

    // Make Minus variation
    rrv.setVal(std::max(cenVal-errVal, rrv.getMin())) ;
    minusVar.push_back(getVal(nset)) ;

    rrv.setVal(cenVal) ;
  }

  // Re-evaluate this RooAbsReal with the central parameters just to be
  // extra-safe that a call to `getPropagatedError()` doesn't change any state.
  // It should not be necessary because thanks to the dirty flag propagation
  // the RooAbsReal is re-evaluated anyway the next time getVal() is called.
  // Still there are imaginable corner cases where it would not be triggered,
  // for example if the user changes the RooFit operation more after the error
  // propagation.
  getVal(nset);

  TMatrixDSym C(paramList.size()) ;
  std::vector<double> errVec(paramList.size()) ;
  for (std::size_t i=0 ; i<paramList.size() ; i++) {
    errVec[i] = std::sqrt(V(i,i)) ;
    for (std::size_t j=i ; j<paramList.size() ; j++) {
      C(i,j) = V(i,j) / std::sqrt(V(i,i)*V(j,j));
      C(j,i) = C(i,j) ;
    }
  }

  // Make std::vector of variations
  TVectorD F(plusVar.size()) ;
  for (std::size_t j=0 ; j<plusVar.size() ; j++) {
    F[j] = (plusVar[j]-minusVar[j]) * 0.5;
  }

  // Calculate error in linear approximation from variations and correlation coefficient
  double sum = F*(C*F) ;

  return sqrt(sum) ;
}



////////////////////////////////////////////////////////////////////////////////
/// Plot function or PDF on frame with support for visualization of the uncertainty encoded in the given fit result fr.
/// \param[in] frame RooPlot to plot on
/// \param[in] fr The RooFitResult, where errors can be extracted
/// \param[in] Z  The desired significance (width) of the error band
/// \param[in] params If non-zero, consider only the subset of the parameters in fr for the error evaluation
/// \param[in] argList Optional `RooCmdArg` that can be applied to a regular plotOn() operation
/// \param[in] linMethod By default (linMethod=true), a linearized error is shown.
/// \return The RooPlot the band was plotted on (for chaining of plotting commands).
///
/// The linearized error is calculated as follows:
/// \f[
///   \mathrm{error}(x) = Z * F_a(x) * \mathrm{Corr}(a,a') * F_{a'}^\mathrm{T}(x),
/// \f]
///
/// where
/// \f[
///     F_a(x) = \frac{ f(x,a+\mathrm{d}a) - f(x,a-\mathrm{d}a) }{2},
/// \f]
/// with \f$ f(x) \f$ the plotted curve and \f$ \mathrm{d}a \f$ taken from the fit result, and
/// \f$ \mathrm{Corr}(a,a') \f$ = the correlation matrix from the fit result, and \f$ Z \f$ = requested signifance (\f$ Z \sigma \f$ band)
///
/// The linear method is fast (required 2*N evaluations of the curve, where N is the number of parameters), but may
/// not be accurate in the presence of strong correlations (~>0.9) and at Z>2 due to linear and Gaussian approximations made
///
/// Alternatively, a more robust error is calculated using a sampling method. In this method a number of curves
/// is calculated with variations of the parameter values, as drawn from a multi-variate Gaussian p.d.f. that is constructed
/// from the fit results covariance matrix. The error(x) is determined by calculating a central interval that capture N% of the variations
/// for each value of x, where N% is controlled by Z (i.e. Z=1 gives N=68%). The number of sampling curves is chosen to be such
/// that at least 30 curves are expected to be outside the N% interval, and is minimally 100 (e.g. Z=1->Ncurve=100, Z=2->Ncurve=659, Z=3->Ncurve=11111)
/// Intervals from the sampling method can be asymmetric, and may perform better in the presence of strong correlations, but may take (much)
/// longer to calculate.

RooPlot* RooAbsReal::plotOnWithErrorBand(RooPlot* frame,const RooFitResult& fr, double Z,const RooArgSet* params, const RooLinkedList& argList, bool linMethod) const
{
  RooLinkedList plotArgListTmp(argList) ;
  RooCmdConfig::stripCmdList(plotArgListTmp,"VisualizeError,MoveToBack") ;

  // Strip any 'internal normalization' arguments from list
  RooLinkedList plotArgList ;
  for (auto * cmd : static_range_cast<RooCmdArg*>(plotArgListTmp)) {
    if (std::string("Normalization")==cmd->GetName()) {
      if (((RooCmdArg*)cmd)->getInt(1)!=0) {
      } else {
   plotArgList.Add(cmd) ;
      }
    } else {
      plotArgList.Add(cmd) ;
    }
  }

  // Function to plot a single curve, creating a copy of the plotArgList to
  // pass as plot command arguments. The "FillColor" command is removed because
  // it has no effect on plotting single curves and would cause a warning.
  auto plotFunc = [&](RooAbsReal const& absReal) {
    RooLinkedList tmp(plotArgList) ;
    RooCmdConfig::stripCmdList(tmp, "FillColor");
    absReal.plotOn(frame, tmp);
  };

  // Generate central value curve
  plotFunc(*this);
  RooCurve* cenCurve = frame->getCurve() ;
  if(!cenCurve){
    coutE(Plotting) << ClassName() << "::" << GetName() << ":plotOnWithErrorBand: no curve for central value available" << std::endl;
    return frame;
  }
  frame->remove(nullptr,false) ;

  RooCurve* band(nullptr) ;
  if (!linMethod) {

    // *** Interval method ***
    //
    // Make N variations of parameters samples from V and visualize N% central interval where N% is defined from Z

    // Clone self for internal use
    RooAbsReal* cloneFunc = static_cast<RooAbsReal*>(cloneTree()) ;
    RooArgSet cloneParams;
    cloneFunc->getObservables(&fr.floatParsFinal(), cloneParams) ;
    RooArgSet errorParams{cloneParams};
    if(params) {
      // clear and fill errorParams only with parameters that both in params and cloneParams
      cloneParams.selectCommon(*params, errorParams);
    }

    // Generate 100 random parameter points distributed according to fit result covariance matrix
    RooAbsPdf* paramPdf = fr.createHessePdf(errorParams) ;
    Int_t n = Int_t(100./TMath::Erfc(Z/sqrt(2.))) ;
    if (n<100) n=100 ;

    coutI(Plotting) << "RooAbsReal::plotOn(" << GetName() << ") INFO: visualizing " << Z << "-sigma uncertainties in parameters "
        << errorParams << " from fit result " << fr.GetName() << " using " << n << " samplings." << std::endl ;

    // Generate variation curves with above set of parameter values
    double ymin = frame->GetMinimum() ;
    double ymax = frame->GetMaximum() ;
    std::unique_ptr<RooDataSet> generatedData{paramPdf->generate(errorParams,n)};
    std::vector<RooCurve*> cvec ;
    for (int i=0 ; i<generatedData->numEntries() ; i++) {
      cloneParams.assign(*generatedData->get(i)) ;
      plotFunc(*cloneFunc);
      cvec.push_back(frame->getCurve()) ;
      frame->remove(nullptr,false) ;
    }
    frame->SetMinimum(ymin) ;
    frame->SetMaximum(ymax) ;


    // Generate upper and lower curve points from 68% interval around each point of central curve
    band = cenCurve->makeErrorBand(cvec,Z) ;

    // Cleanup
    delete paramPdf ;
    delete cloneFunc ;
    for (std::vector<RooCurve*>::iterator i=cvec.begin() ; i!=cvec.end() ; ++i) {
      delete (*i) ;
    }

  } else {

    // *** Linear Method ***
    //
    // Make a one-sigma up- and down fluctation for each parameter and visualize
    // a from a linearized calculation as follows
    //
    //   error(x) = F(a) C_aa' F(a')
    //
    //   Where F(a) = (f(x,a+da) - f(x,a-da))/2
    //   and C_aa' is the correlation matrix

    // Strip out parameters with zero error
    RooArgList fpf_stripped;
    for (auto const* frv : static_range_cast<RooRealVar*>(fr.floatParsFinal())) {
       if (frv->getError() > frv->getVal() * std::numeric_limits<double>::epsilon()) {
          fpf_stripped.add(*frv);
       }
    }

    // Clone self for internal use
    RooAbsReal* cloneFunc = static_cast<RooAbsReal*>(cloneTree()) ;
    RooArgSet cloneParams;
    cloneFunc->getObservables(&fpf_stripped, cloneParams) ;
    RooArgSet errorParams{cloneParams};
    if(params) {
      // clear and fill errorParams only with parameters that both in params and cloneParams
      cloneParams.selectCommon(*params, errorParams);
    }


    // Make list of parameter instances of cloneFunc in order of error matrix
    RooArgList paramList ;
    const RooArgList& fpf = fr.floatParsFinal() ;
    std::vector<int> fpf_idx ;
    for (std::size_t i=0 ; i<fpf.size() ; i++) {
      RooAbsArg* par = errorParams.find(fpf[i].GetName()) ;
      if (par) {
   paramList.add(*par) ;
   fpf_idx.push_back(i) ;
      }
    }

    std::vector<RooCurve *> plusVar;
    std::vector<RooCurve *> minusVar;

    // Create std::vector of plus,minus variations for each parameter

    TMatrixDSym V(paramList.size() == fr.floatParsFinal().size() ?
        fr.covarianceMatrix():
        fr.reducedCovarianceMatrix(paramList)) ;


    for (std::size_t ivar=0 ; ivar<paramList.size() ; ivar++) {

      RooRealVar& rrv = static_cast<RooRealVar&>(fpf[fpf_idx[ivar]]) ;

      double cenVal = rrv.getVal() ;
      double errVal = sqrt(V(ivar,ivar)) ;

      auto * var = static_cast<RooRealVar*>(paramList.at(ivar));

      // Check if the variations are still in the parameter range, otherwise we
      // can't compute the error bands and get an invalid plot!
      if(!var->inRange(cenVal+Z*errVal, nullptr) || !var->inRange(cenVal-Z*errVal, nullptr)) {
        std::stringstream ss;
        ss << "RooAbsReal::plotOn(" << GetName() << "): the " << Z << "-sigma error band for the parameter "
           << "\"" << var->GetName() << "\" is invalid"
           << " because the variations (" << cenVal-Z*errVal << ", " << cenVal+Z*errVal
           << ") are outside the defined range [" << var->getMin() << ", " << var->getMax() << "]!\n"
           << "                         The variations will be clipped inside the range."
           << " This might or might not be acceptable in your usecase.";
        coutW(Plotting) << ss.str() << std::endl;
      }

      // Make Plus variation
      var->setVal(std::min(cenVal+Z*errVal, var->getMax()));

      plotFunc(*cloneFunc);
      plusVar.push_back(frame->getCurve()) ;
      frame->remove(nullptr,false) ;


      // Make Minus variation
      var->setVal(std::max(cenVal-Z*errVal, var->getMin()));
      plotFunc(*cloneFunc);
      minusVar.push_back(frame->getCurve()) ;
      frame->remove(nullptr,false) ;

      var->setVal(cenVal) ;
    }

    TMatrixDSym C(paramList.size()) ;
    std::vector<double> errVec(paramList.size()) ;
    for (std::size_t i=0 ; i<paramList.size() ; i++) {
      errVec[i] = sqrt(V(i,i)) ;
      for (std::size_t j=i ; j<paramList.size() ; j++) {
   C(i,j) = V(i,j)/sqrt(V(i,i)*V(j,j)) ;
   C(j,i) = C(i,j) ;
      }
    }

    band = cenCurve->makeErrorBand(plusVar,minusVar,C,Z) ;


    // Cleanup
    delete cloneFunc ;
    for (std::vector<RooCurve*>::iterator i=plusVar.begin() ; i!=plusVar.end() ; ++i) {
      delete (*i) ;
    }
    for (std::vector<RooCurve*>::iterator i=minusVar.begin() ; i!=minusVar.end() ; ++i) {
      delete (*i) ;
    }

  }

  delete cenCurve ;
  if (!band) return frame ;

  // Define configuration for this method
  RooCmdConfig pc("RooAbsPdf::plotOn(" + std::string(GetName()) + ")");
  pc.defineString("drawOption","DrawOption",0,"F") ;
  pc.defineString("curveNameSuffix","CurveNameSuffix",0,"") ;
  pc.defineInt("lineColor","LineColor",0,-999) ;
  pc.defineInt("lineStyle","LineStyle",0,-999) ;
  pc.defineInt("lineWidth","LineWidth",0,-999) ;
  pc.defineInt("markerColor","MarkerColor",0,-999) ;
  pc.defineInt("markerStyle","MarkerStyle",0,-999) ;
  pc.defineDouble("markerSize","MarkerSize",0,-999) ;
  pc.defineInt("fillColor","FillColor",0,-999) ;
  pc.defineInt("fillStyle","FillStyle",0,-999) ;
  pc.defineString("curveName","Name",0,"") ;
  pc.defineInt("curveInvisible","Invisible",0,0) ;
  pc.defineInt("moveToBack","MoveToBack",0,0) ;
  pc.allowUndefined() ;

  // Process & check varargs
  pc.process(argList) ;
  if (!pc.ok(true)) {
    return frame ;
  }

  // Insert error band in plot frame
  frame->addPlotable(band,pc.getString("drawOption"),pc.getInt("curveInvisible")) ;

  // Optionally adjust line/fill attributes
  Int_t lineColor = pc.getInt("lineColor") ;
  Int_t lineStyle = pc.getInt("lineStyle") ;
  Int_t lineWidth = pc.getInt("lineWidth") ;
  Int_t markerColor = pc.getInt("markerColor") ;
  Int_t markerStyle = pc.getInt("markerStyle") ;
  Size_t markerSize  = pc.getDouble("markerSize") ;
  Int_t fillColor = pc.getInt("fillColor") ;
  Int_t fillStyle = pc.getInt("fillStyle") ;
  if (lineColor!=-999) frame->getAttLine()->SetLineColor(lineColor) ;
  if (lineStyle!=-999) frame->getAttLine()->SetLineStyle(lineStyle) ;
  if (lineWidth!=-999) frame->getAttLine()->SetLineWidth(lineWidth) ;
  if (fillColor!=-999) frame->getAttFill()->SetFillColor(fillColor) ;
  if (fillStyle!=-999) frame->getAttFill()->SetFillStyle(fillStyle) ;
  if (markerColor!=-999) frame->getAttMarker()->SetMarkerColor(markerColor) ;
  if (markerStyle!=-999) frame->getAttMarker()->SetMarkerStyle(markerStyle) ;
  if (markerSize!=-999) frame->getAttMarker()->SetMarkerSize(markerSize) ;

  // Adjust name if requested
  if (pc.getString("curveName",nullptr,true)) {
    band->SetName(pc.getString("curveName",nullptr,true)) ;
  } else if (pc.getString("curveNameSuffix",nullptr,true)) {
    TString name(band->GetName()) ;
    name.Append(pc.getString("curveNameSuffix",nullptr,true)) ;
    band->SetName(name.Data()) ;
  }

  // Move last inserted object to back to drawing stack if requested
  if (pc.getInt("moveToBack") && frame->numItems()>1) {
    frame->drawBefore(frame->getObject(0)->GetName(), frame->getCurve()->GetName());
  }


  return frame ;
}




////////////////////////////////////////////////////////////////////////////////
/// Utility function for plotOn(), perform general sanity check on frame to ensure safe plotting operations

bool RooAbsReal::plotSanityChecks(RooPlot* frame) const
{
  // check that we are passed a valid plot frame to use
  if(nullptr == frame) {
    coutE(Plotting) << ClassName() << "::" << GetName() << ":plotOn: frame is null" << std::endl;
    return true;
  }

  // check that this frame knows what variable to plot
  RooAbsReal* var = frame->getPlotVar() ;
  if(!var) {
    coutE(Plotting) << ClassName() << "::" << GetName()
    << ":plotOn: frame does not specify a plot variable" << std::endl;
    return true;
  }

  // check that the plot variable is not derived
  if(!dynamic_cast<RooAbsRealLValue*>(var)) {
    coutE(Plotting) << ClassName() << "::" << GetName() << ":plotOn: cannot plot variable \""
          << var->GetName() << "\" of type " << var->ClassName() << std::endl;
    return true;
  }

  // check if we actually depend on the plot variable
  if(!this->dependsOn(*var)) {
    coutE(Plotting) << ClassName() << "::" << GetName() << ":plotOn: WARNING: variable is not an explicit dependent: "
          << var->GetName() << std::endl;
  }

  return false ;
}




////////////////////////////////////////////////////////////////////////////////
/// Utility function for plotOn() that constructs the set of
/// observables to project when plotting ourselves as function of
/// 'plotVar'. 'allVars' is the list of variables that must be
/// projected, but may contain variables that we do not depend on. If
/// 'silent' is cleared, warnings about inconsistent input parameters
/// will be printed.

void RooAbsReal::makeProjectionSet(const RooAbsArg* plotVar, const RooArgSet* allVars,
               RooArgSet& projectedVars, bool silent) const
{
  cxcoutD(Plotting) << "RooAbsReal::makeProjectionSet(" << GetName() << ") plotVar = " << plotVar->GetName()
          << " allVars = " << (allVars?(*allVars):RooArgSet()) << std::endl ;

  projectedVars.removeAll() ;
  if (!allVars) return ;

  // Start out with suggested list of variables
  projectedVars.add(*allVars) ;

  // Take out plot variable
  RooAbsArg *found= projectedVars.find(plotVar->GetName());
  if(found) {
    projectedVars.remove(*found);

    // Take out eventual servers of plotVar
    std::unique_ptr<RooArgSet> plotServers{plotVar->getObservables(&projectedVars)};
    for(RooAbsArg * ps : *plotServers) {
      RooAbsArg* tmp = projectedVars.find(ps->GetName()) ;
      if (tmp) {
   cxcoutD(Plotting) << "RooAbsReal::makeProjectionSet(" << GetName() << ") removing " << tmp->GetName()
           << " from projection set because it a server of " << plotVar->GetName() << std::endl ;
   projectedVars.remove(*tmp) ;
      }
    }

    if (!silent) {
      coutW(Plotting) << "RooAbsReal::plotOn(" << GetName()
            << ") WARNING: cannot project out frame variable ("
            << found->GetName() << "), ignoring" << std::endl ;
    }
  }

  // Take out all non-dependents of function
  for(RooAbsArg * arg : *allVars) {
    if (!dependsOnValue(*arg)) {
      projectedVars.remove(*arg,true) ;

      cxcoutD(Plotting) << "RooAbsReal::plotOn(" << GetName()
         << ") function doesn't depend on projection variable "
         << arg->GetName() << ", ignoring" << std::endl ;
    }
  }
}




////////////////////////////////////////////////////////////////////////////////
/// If true, the current pdf is a selected component (for use in plotting)

bool RooAbsReal::isSelectedComp() const
{
  return _selectComp || _globalSelectComp ;
}



////////////////////////////////////////////////////////////////////////////////
/// Global switch controlling the activation of the selectComp() functionality

void RooAbsReal::globalSelectComp(bool flag)
{
  _globalSelectComp = flag ;
}




////////////////////////////////////////////////////////////////////////////////
/// Create an interface adaptor f(vars) that binds us to the specified variables
/// (in arbitrary order). For example, calling bindVars({x1,x3}) on an object
/// F(x1,x2,x3,x4) returns an object f(x1,x3) that is evaluated using the
/// current values of x2 and x4. The caller takes ownership of the returned adaptor.

RooFit::OwningPtr<RooAbsFunc> RooAbsReal::bindVars(const RooArgSet &vars, const RooArgSet* nset, bool clipInvalid) const
{
  auto binding = std::make_unique<RooRealBinding>(*this,vars,nset,clipInvalid);
  if(!binding->isValid()) {
    coutE(InputArguments) << ClassName() << "::" << GetName() << ":bindVars: cannot bind to " << vars << std::endl ;
    return nullptr;
  }
  return RooFit::makeOwningPtr(std::unique_ptr<RooAbsFunc>{std::move(binding)});
}



////////////////////////////////////////////////////////////////////////////////
/// Copy the cached value of another RooAbsArg to our cache.
/// Warning: This function just copies the cached values of source,
/// it is the callers responsibility to make sure the cache is clean.

void RooAbsReal::copyCache(const RooAbsArg* source, bool /*valueOnly*/, bool setValDirty)
{
  auto other = static_cast<const RooAbsReal*>(source);
  assert(dynamic_cast<const RooAbsReal*>(source));

  _value = other->_treeReadBuffer ? other->_treeReadBuffer->operator double() : other->_value;

  if (setValDirty) {
    setValueDirty() ;
  }
}


////////////////////////////////////////////////////////////////////////////////

void RooAbsReal::attachToVStore(RooVectorDataStore& vstore)
{
  vstore.addReal(this)->setBuffer(this,&_value);
}


////////////////////////////////////////////////////////////////////////////////
/// Attach object to a branch of given TTree. By default it will
/// register the internal value cache RooAbsReal::_value as branch
/// buffer for a double tree branch with the same name as this
/// object. If no double branch is found with the name of this
/// object, this method looks for a Float_t Int_t, UChar_t and UInt_t, etc
/// branch. If any of these are found, a TreeReadBuffer
/// that branch is created, and saved in _treeReadBuffer.
/// TreeReadBuffer::operator double() can be used to convert the values.
/// This is used by copyCache().
void RooAbsReal::attachToTree(TTree& t, Int_t bufSize)
{
  // First determine if branch is taken
  TString cleanName(cleanBranchName()) ;
  TBranch* branch = t.GetBranch(cleanName) ;
  if (branch) {

    // Determine if existing branch is Float_t or double
    TLeaf* leaf = static_cast<TLeaf*>(branch->GetListOfLeaves()->At(0)) ;

    // Check that leaf is _not_ an array
    Int_t dummy ;
    TLeaf* counterLeaf = leaf->GetLeafCounter(dummy) ;
    if (counterLeaf) {
      coutE(Eval) << "RooAbsReal::attachToTree(" << GetName() << ") ERROR: TTree branch " << GetName()
        << " is an array and cannot be attached to a RooAbsReal" << std::endl ;
      return ;
    }

    TString typeName(leaf->GetTypeName()) ;


    // For different type names, store three items:
    // first: A tag attached to this instance. Not used inside RooFit, any more, but users might rely on it.
    // second: A function to attach
    std::map<std::string, std::pair<std::string, std::function<std::unique_ptr<TreeReadBuffer>()>>> typeMap {
      {"Float_t",   {"FLOAT_TREE_BRANCH",            [&](){ return createTreeReadBuffer<Float_t  >(cleanName, t); }}},
      {"Int_t",     {"INTEGER_TREE_BRANCH",          [&](){ return createTreeReadBuffer<Int_t    >(cleanName, t); }}},
      {"UChar_t",   {"BYTE_TREE_BRANCH",             [&](){ return createTreeReadBuffer<UChar_t  >(cleanName, t); }}},
      {"Bool_t",    {"BOOL_TREE_BRANCH",             [&](){ return createTreeReadBuffer<Bool_t   >(cleanName, t); }}},
      {"Char_t",    {"SIGNEDBYTE_TREE_BRANCH",       [&](){ return createTreeReadBuffer<Char_t   >(cleanName, t); }}},
      {"UInt_t",    {"UNSIGNED_INTEGER_TREE_BRANCH", [&](){ return createTreeReadBuffer<UInt_t   >(cleanName, t); }}},
      {"Long64_t",  {"LONG_TREE_BRANCH",             [&](){ return createTreeReadBuffer<Long64_t >(cleanName, t); }}},
      {"ULong64_t", {"UNSIGNED_LONG_TREE_BRANCH",    [&](){ return createTreeReadBuffer<ULong64_t>(cleanName, t); }}},
      {"Short_t",   {"SHORT_TREE_BRANCH",            [&](){ return createTreeReadBuffer<Short_t  >(cleanName, t); }}},
      {"UShort_t",  {"UNSIGNED_SHORT_TREE_BRANCH",   [&](){ return createTreeReadBuffer<UShort_t >(cleanName, t); }}},
    };

    auto typeDetails = typeMap.find(typeName.Data());
    if (typeDetails != typeMap.end()) {
      coutI(DataHandling) << "RooAbsReal::attachToTree(" << GetName() << ") TTree " << typeDetails->first << " branch " << GetName()
                  << " will be converted to double precision." << std::endl ;
      setAttribute(typeDetails->second.first.c_str(), true);
      _treeReadBuffer = typeDetails->second.second().release();
    } else {
      if (_treeReadBuffer) {
         delete _treeReadBuffer;
      }
      _treeReadBuffer = nullptr;

      if (!typeName.CompareTo("Double_t")) {
        t.SetBranchAddress(cleanName, &_value);
      }
      else {
        coutE(InputArguments) << "RooAbsReal::attachToTree(" << GetName() << ") data type " << typeName << " is not supported." << std::endl ;
      }
    }
  } else {

    TString format(cleanName);
    format.Append("/D");
    branch = t.Branch(cleanName, &_value, (const Text_t*)format, bufSize);
  }

}



////////////////////////////////////////////////////////////////////////////////
/// Fill the tree branch that associated with this object with its current value

void RooAbsReal::fillTreeBranch(TTree& t)
{
  // First determine if branch is taken
  TBranch* branch = t.GetBranch(cleanBranchName()) ;
  if (!branch) {
    coutE(Eval) << "RooAbsReal::fillTreeBranch(" << GetName() << ") ERROR: not attached to tree: " << cleanBranchName() << std::endl ;
    assert(0) ;
  }
  branch->Fill() ;

}



////////////////////////////////////////////////////////////////////////////////
/// (De)Activate associated tree branch

void RooAbsReal::setTreeBranchStatus(TTree& t, bool active)
{
  TBranch* branch = t.GetBranch(cleanBranchName()) ;
  if (branch) {
    t.SetBranchStatus(cleanBranchName(),active?true:false) ;
  }
}



////////////////////////////////////////////////////////////////////////////////
/// Create a RooRealVar fundamental object with our properties. The new
/// object will be created without any fit limits.

RooFit::OwningPtr<RooAbsArg> RooAbsReal::createFundamental(const char* newname) const
{
  auto fund = std::make_unique<RooRealVar>(newname?newname:GetName(),GetTitle(),_value,getUnit());
  fund->removeRange();
  fund->setPlotLabel(getPlotLabel());
  fund->setAttribute("fundamentalCopy");
  return RooFit::makeOwningPtr<RooAbsArg>(std::move(fund));
}

////////////////////////////////////////////////////////////////////////////////
/// Utility function for use in getAnalyticalIntegral(). If the
/// contents of 'refset' occur in set 'allDeps' then the arguments
/// held in 'refset' are copied from allDeps to analDeps.

bool RooAbsReal::matchArgs(const RooArgSet& allDeps, RooArgSet& analDeps,
              const RooArgSet& refset) const
{
  TList nameList ;
  for(RooAbsArg * arg : refset) {
    nameList.Add(new TObjString(arg->GetName())) ;
  }

  bool result = matchArgsByName(allDeps,analDeps,nameList) ;
  nameList.Delete() ;
  return result ;
}



////////////////////////////////////////////////////////////////////////////////
/// Check if allArgs contains matching elements for each name in nameList. If it does,
/// add the corresponding args from allArgs to matchedArgs and return true. Otherwise
/// return false and do not change matchedArgs.

bool RooAbsReal::matchArgsByName(const RooArgSet &allArgs, RooArgSet &matchedArgs,
              const TList &nameList) const
{
  RooArgSet matched("matched");
  bool isMatched(true);
  for(auto * name : static_range_cast<TObjString*>(nameList)) {
    RooAbsArg *found= allArgs.find(name->String().Data());
    if(found) {
      matched.add(*found);
    }
    else {
      isMatched= false;
      break;
    }
  }

  // nameList may not contain multiple entries with the same name
  // that are both matched
  if (isMatched && int(matched.size())!=nameList.GetSize()) {
    isMatched = false ;
  }

  if(isMatched) matchedArgs.add(matched);
  return isMatched;
}



////////////////////////////////////////////////////////////////////////////////
/// Returns the default numeric integration configuration for all RooAbsReals

RooNumIntConfig* RooAbsReal::defaultIntegratorConfig()
{
  return &RooNumIntConfig::defaultConfig() ;
}


////////////////////////////////////////////////////////////////////////////////
/// Returns the specialized integrator configuration for _this_ RooAbsReal.
/// If this object has no specialized configuration, a null pointer is returned.

RooNumIntConfig* RooAbsReal::specialIntegratorConfig() const
{
  return _specIntegratorConfig.get();
}


////////////////////////////////////////////////////////////////////////////////
/// Returns the specialized integrator configuration for _this_ RooAbsReal.
/// If this object has no specialized configuration, a null pointer is returned,
/// unless createOnTheFly is true in which case a clone of the default integrator
/// configuration is created, installed as specialized configuration, and returned

RooNumIntConfig* RooAbsReal::specialIntegratorConfig(bool createOnTheFly)
{
  if (!_specIntegratorConfig && createOnTheFly) {
    _specIntegratorConfig = std::make_unique<RooNumIntConfig>(*defaultIntegratorConfig()) ;
  }
  return _specIntegratorConfig.get();
}



////////////////////////////////////////////////////////////////////////////////
/// Return the numeric integration configuration used for this object. If
/// a specialized configuration was associated with this object, that configuration
/// is returned, otherwise the default configuration for all RooAbsReals is returned

const RooNumIntConfig* RooAbsReal::getIntegratorConfig() const
{
  const RooNumIntConfig* config = specialIntegratorConfig() ;
  return config ? config : defaultIntegratorConfig();
}


////////////////////////////////////////////////////////////////////////////////
/// Return the numeric integration configuration used for this object. If
/// a specialized configuration was associated with this object, that configuration
/// is returned, otherwise the default configuration for all RooAbsReals is returned

RooNumIntConfig* RooAbsReal::getIntegratorConfig()
{
  RooNumIntConfig* config = specialIntegratorConfig() ;
  return config ? config : defaultIntegratorConfig();
}



////////////////////////////////////////////////////////////////////////////////
/// Set the given integrator configuration as default numeric integration
/// configuration for this object

void RooAbsReal::setIntegratorConfig(const RooNumIntConfig& config)
{
  _specIntegratorConfig = std::make_unique<RooNumIntConfig>(config);
}



////////////////////////////////////////////////////////////////////////////////
/// Remove the specialized numeric integration configuration associated
/// with this object

void RooAbsReal::setIntegratorConfig()
{
  _specIntegratorConfig.reset();
}

////////////////////////////////////////////////////////////////////////////////
/// Interface function to force use of a given set of observables
/// to interpret function value. Needed for functions or p.d.f.s
/// whose shape depends on the choice of normalization such as
/// RooAddPdf

void RooAbsReal::selectNormalization(const RooArgSet *, bool) {}

////////////////////////////////////////////////////////////////////////////////
/// Interface function to force use of a given normalization range
/// to interpret function value. Needed for functions or p.d.f.s
/// whose shape depends on the choice of normalization such as
/// RooAddPdf

void RooAbsReal::selectNormalizationRange(const char *, bool) {}

////////////////////////////////////////////////////////////////////////////////
/// Advertise capability to determine maximum value of function for given set of
/// observables. If no direct generator method is provided, this information
/// will assist the accept/reject generator to operate more efficiently as
/// it can skip the initial trial sampling phase to empirically find the function
/// maximum

Int_t RooAbsReal::getMaxVal(const RooArgSet & /*vars*/) const
{
   return 0;
}

////////////////////////////////////////////////////////////////////////////////
/// Return maximum value for set of observables identified by code assigned
/// in getMaxVal

double RooAbsReal::maxVal(Int_t /*code*/) const
{
  assert(1) ;
  return 0 ;
}



////////////////////////////////////////////////////////////////////////////////
/// Interface to insert remote error logging messages received by RooRealMPFE into current error logging stream.

void RooAbsReal::logEvalError(const RooAbsReal* originator, const char* origName, const char* message, const char* serverValueString)
{
  if (evalErrorData().mode == Ignore) {
    return ;
  }

  if (evalErrorData().mode == CountErrors) {
    evalErrorData().count++ ;
    return ;
  }

  static bool inLogEvalError = false ;

  if (inLogEvalError) {
    return ;
  }
  inLogEvalError = true ;

  EvalError ee ;
  ee.setMessage(message) ;

  if (serverValueString) {
    ee.setServerValues(serverValueString) ;
  }

  if (evalErrorData().mode == PrintErrors) {
   oocoutE(nullptr,Eval) << "RooAbsReal::logEvalError(" << "<STATIC>" << ") evaluation error, " << std::endl
         << " origin       : " << origName << std::endl
         << " message      : " << ee._msg << std::endl
         << " server values: " << ee._srvval << std::endl ;
  } else if (evalErrorData().mode == CollectErrors) {
    auto &evalErrorList = evalErrorData().errorList[originator];
    evalErrorList.first = origName ;
    evalErrorList.second.push_back(ee) ;
  }


  inLogEvalError = false ;
}



////////////////////////////////////////////////////////////////////////////////
/// Log evaluation error message. Evaluation errors may be routed through a different
/// protocol than generic RooFit warning message (which go straight through RooMsgService)
/// because evaluation errors can occur in very large numbers in the use of likelihood
/// evaluations. In logEvalError mode, controlled by global method enableEvalErrorLogging()
/// messages reported through this function are not printed but all stored in a list,
/// along with server values at the time of reporting. Error messages logged in this
/// way can be printed in a structured way, eliminating duplicates and with the ability
/// to truncate the list by printEvalErrors. This is the standard mode of error logging
/// during MINUIT operations. If enableEvalErrorLogging() is false, all errors
/// reported through this method are passed for immediate printing through RooMsgService.
/// A string with server names and values is constructed automatically for error logging
/// purposes, unless a custom string with similar information is passed as argument.

void RooAbsReal::logEvalError(const char* message, const char* serverValueString) const
{
  if (evalErrorData().mode == Ignore) {
    return ;
  }

  if (evalErrorData().mode == CountErrors) {
    evalErrorData().count++ ;
    return ;
  }

  static bool inLogEvalError = false ;

  if (inLogEvalError) {
    return ;
  }
  inLogEvalError = true ;

  EvalError ee ;
  ee.setMessage(message) ;

  if (serverValueString) {
    ee.setServerValues(serverValueString) ;
  } else {
    std::string srvval ;
    std::ostringstream oss ;
    bool first(true) ;
    for (Int_t i=0 ; i<numProxies() ; i++) {
      RooAbsProxy* p = getProxy(i) ;
      if (!p) continue ;
      //if (p->name()[0]=='!') continue ;
      if (first) {
   first=false ;
      } else {
   oss << ", " ;
      }
      p->print(oss,true) ;
    }
    ee.setServerValues(oss.str().c_str()) ;
  }

  std::ostringstream oss2 ;
  printStream(oss2,kName|kClassName|kArgs,kInline)  ;

  if (evalErrorData().mode == PrintErrors) {
   coutE(Eval) << "RooAbsReal::logEvalError(" << GetName() << ") evaluation error, " << std::endl
          << " origin       : " << oss2.str() << std::endl
          << " message      : " << ee._msg << std::endl
          << " server values: " << ee._srvval << std::endl ;
  } else if (evalErrorData().mode == CollectErrors) {
    auto &evalErrorList = evalErrorData().errorList[this];
    if (evalErrorList.second.size() >= 2048) {
       // avoid overflowing the error list, so if there are very many, print
       // the oldest one first, and pop it off the list
       const EvalError& oee = evalErrorList.second.front();
       // print to debug stream, since these would normally be suppressed, and
       // we do not want to increase the error count in the message service...
       ccoutD(Eval) << "RooAbsReal::logEvalError(" << GetName()
              << ") delayed evaluation error, " << std::endl
                   << " origin       : " << oss2.str() << std::endl
                   << " message      : " << oee._msg << std::endl
                   << " server values: " << oee._srvval << std::endl ;
       evalErrorList.second.pop_front();
    }
    evalErrorList.first = oss2.str() ;
    evalErrorList.second.push_back(ee) ;
  }

  inLogEvalError = false ;
  //coutE(Tracing) << "RooAbsReal::logEvalError(" << GetName() << ") message = " << message << std::endl ;
}




////////////////////////////////////////////////////////////////////////////////
/// Clear the stack of evaluation error messages

void RooAbsReal::clearEvalErrorLog()
{
  if (evalErrorData().mode == PrintErrors) {
    return ;
  } else if (evalErrorData().mode == CollectErrors) {
    evalErrorData().errorList.clear() ;
  } else {
    evalErrorData().count = 0 ;
  }
}


////////////////////////////////////////////////////////////////////////////////
/// Retrieve bin boundaries if this distribution is binned in `obs`.
/// \param[in] obs Observable to retrieve boundaries for.
/// \param[in] xlo Beginning of range.
/// \param[in] xhi End of range.
/// \return The caller owns the returned std::list.
std::list<double>* RooAbsReal::binBoundaries(RooAbsRealLValue& /*obs*/, double /*xlo*/, double /*xhi*/) const {
  return nullptr;
}


////////////////////////////////////////////////////////////////////////////////
/// Interface for returning an optional hint for initial sampling points when constructing a curve projected on observable `obs`.
/// \param[in] obs Observable to retrieve sampling hint for.
/// \param[in] xlo Beginning of range.
/// \param[in] xhi End of range.
/// \return The caller owns the returned std::list.
std::list<double>* RooAbsReal::plotSamplingHint(RooAbsRealLValue& /*obs*/, double /*xlo*/, double /*xhi*/) const {
  return nullptr;
}

////////////////////////////////////////////////////////////////////////////////
/// Print all outstanding logged evaluation error on the given ostream. If maxPerNode
/// is zero, only the number of errors for each source (object with unique name) is listed.
/// If maxPerNode is greater than zero, up to maxPerNode detailed error messages are shown
/// per source of errors. A truncation message is shown if there were more errors logged
/// than shown.

void RooAbsReal::printEvalErrors(std::ostream &os, Int_t maxPerNode)
{
   if (evalErrorData().mode == CountErrors) {
      os << evalErrorData().count << " errors counted" << std::endl;
   }

   if (maxPerNode < 0)
      return;

   for (auto const &item : evalErrorData().errorList) {
      if (maxPerNode == 0) {

         // Only print node name with total number of errors
         os << item.second.first;
         // item.first->printStream(os,kName|kClassName|kArgs,kInline)  ;
         os << " has " << item.second.second.size() << " errors" << std::endl;

      } else {

         // Print node name and details of 'maxPerNode' errors
         os << item.second.first << std::endl;
         // item.first->printStream(os,kName|kClassName|kArgs,kSingleLine) ;

         Int_t i(0);
         for (auto const &item2 : item.second.second) {
            os << "     " << item2._msg << " @ " << item2._srvval << std::endl;
            if (i > maxPerNode) {
               os << "    ... (remaining " << item.second.second.size() - maxPerNode << " messages suppressed)"
                  << std::endl;
               break;
            }
            i++;
         }
      }
   }
}



////////////////////////////////////////////////////////////////////////////////
/// Return the number of logged evaluation errors since the last clearing.

Int_t RooAbsReal::numEvalErrors()
{
   auto &evalErrors = evalErrorData();
   if (evalErrors.mode == CountErrors) {
      return evalErrors.count;
   }

   Int_t ntot(0);
   for (auto const &elem : evalErrors.errorList) {
      ntot += elem.second.second.size();
   }
   return ntot;
}



////////////////////////////////////////////////////////////////////////////////
/// Fix the interpretation of the coefficient of any RooAddPdf component in
/// the expression tree headed by this object to the given set of observables.
///
/// If the force flag is false, the normalization choice is only fixed for those
/// RooAddPdf components that have the default 'automatic' interpretation of
/// coefficients (i.e. the interpretation is defined by the observables passed
/// to getVal()). If force is true, also RooAddPdf that already have a fixed
/// interpretation are changed to a new fixed interpretation.

void RooAbsReal::fixAddCoefNormalization(const RooArgSet& addNormSet, bool force)
{
  std::unique_ptr<RooArgSet> compSet{getComponents()};
  for(auto * pdf : dynamic_range_cast<RooAbsPdf*>(*compSet)) {
    if (pdf) {
      pdf->selectNormalization(addNormSet.empty() ? nullptr : &addNormSet,force);
    }
  }
}



////////////////////////////////////////////////////////////////////////////////
/// Fix the interpretation of the coefficient of any RooAddPdf component in
/// the expression tree headed by this object to the given set of observables.
///
/// If the force flag is false, the normalization range choice is only fixed for those
/// RooAddPdf components that currently use the default full domain to interpret their
/// coefficients. If force is true, also RooAddPdf that already have a fixed
/// interpretation range are changed to a new fixed interpretation range.

void RooAbsReal::fixAddCoefRange(const char* rangeName, bool force)
{
  std::unique_ptr<RooArgSet> compSet{getComponents()};
  for(auto * pdf : dynamic_range_cast<RooAbsPdf*>(*compSet)) {
    if (pdf) {
      pdf->selectNormalizationRange(rangeName,force) ;
    }
  }
}



////////////////////////////////////////////////////////////////////////////////
/// Interface method for function objects to indicate their preferred order of observables
/// for scanning their values into a (multi-dimensional) histogram or RooDataSet. The observables
/// to be ordered are offered in argument 'obs' and should be copied in their preferred
/// order into argument 'orderedObs', This default implementation indicates no preference
/// and copies the original order of 'obs' into 'orderedObs'

void RooAbsReal::preferredObservableScanOrder(const RooArgSet& obs, RooArgSet& orderedObs) const
{
  // Dummy implementation, do nothing
  orderedObs.removeAll() ;
  orderedObs.add(obs) ;
}



////////////////////////////////////////////////////////////////////////////////
/// Calls createRunningIntegral(const RooArgSet&, const RooCmdArg&, const RooCmdArg&, const RooCmdArg&, const RooCmdArg&, const RooCmdArg&, const RooCmdArg&, const RooCmdArg&, const RooCmdArg&)

RooFit::OwningPtr<RooAbsReal> RooAbsReal::createRunningIntegral(const RooArgSet& iset, const RooArgSet& nset)
{
  return createRunningIntegral(iset,RooFit::SupNormSet(nset)) ;
}



////////////////////////////////////////////////////////////////////////////////
/// Create an object that represents the running integral of the function over one or more observables listed in iset, i.e.
/// \f[
///   \int_{x_\mathrm{lo}}^x f(x') \, \mathrm{d}x'
/// \f]
///
/// The actual integration calculation is only performed when the return object is evaluated. The name
/// of the integral object is automatically constructed from the name of the input function, the variables
/// it integrates and the range integrates over. The default strategy to calculate the running integrals is
///
///   - If the integrand (this object) supports analytical integration, construct an integral object
///     that calculate the running integrals value by calculating the analytical integral each
///     time the running integral object is evaluated
///
///   - If the integrand (this object) requires numeric integration to construct the running integral
///     create an object of class RooNumRunningInt which first samples the entire function and integrates
///     the sampled function numerically. This method has superior performance as there is no need to
///     perform a full (numeric) integration for each evaluation of the running integral object, but
///     only when one of its parameters has changed.
///
/// The choice of strategy can be changed with the ScanAll() argument, which forces the use of the
/// scanning technique implemented in RooNumRunningInt for all use cases, and with the ScanNone()
/// argument which forces the 'integrate each evaluation' technique for all use cases. The sampling
/// granularity for the scanning technique can be controlled with the ScanParameters technique
/// which allows to specify the number of samples to be taken, and to which order the resulting
/// running integral should be interpolated. The default values are 1000 samples and 2nd order
/// interpolation.
///
/// The following named arguments are accepted
/// | | Effect on integral creation
/// |-|-------------------------------
/// | `SupNormSet(const RooArgSet&)`         | Observables over which should be normalized _in addition_ to the integration observables
/// | `ScanParameters(Int_t nbins, Int_t intOrder)`    | Parameters for scanning technique of making CDF: number of sampled bins and order of interpolation applied on numeric cdf
/// | `ScanNum()`                            | Apply scanning technique if cdf integral involves numeric integration
/// | `ScanAll()`                            | Always apply scanning technique
/// | `ScanNone()`                           | Never apply scanning technique

RooFit::OwningPtr<RooAbsReal> RooAbsReal::createRunningIntegral(const RooArgSet& iset, const RooCmdArg& arg1, const RooCmdArg& arg2,
             const RooCmdArg& arg3, const RooCmdArg& arg4, const RooCmdArg& arg5,
             const RooCmdArg& arg6, const RooCmdArg& arg7, const RooCmdArg& arg8)
{
  // Define configuration for this method
  RooCmdConfig pc("RooAbsReal::createRunningIntegral(" + std::string(GetName()) + ")");
  pc.defineSet("supNormSet","SupNormSet",0,nullptr) ;
  pc.defineInt("numScanBins","ScanParameters",0,1000) ;
  pc.defineInt("intOrder","ScanParameters",1,2) ;
  pc.defineInt("doScanNum","ScanNum",0,1) ;
  pc.defineInt("doScanAll","ScanAll",0,0) ;
  pc.defineInt("doScanNon","ScanNone",0,0) ;
  pc.defineMutex("ScanNum","ScanAll","ScanNone") ;

  // Process & check varargs
  pc.process(arg1,arg2,arg3,arg4,arg5,arg6,arg7,arg8) ;
  if (!pc.ok(true)) {
    return nullptr ;
  }

  // Extract values from named arguments
  RooArgSet nset ;
  if (const RooArgSet* snset = pc.getSet("supNormSet",nullptr)) {
    nset.add(*snset) ;
  }
  Int_t numScanBins = pc.getInt("numScanBins") ;
  Int_t intOrder = pc.getInt("intOrder") ;
  Int_t doScanNum = pc.getInt("doScanNum") ;
  Int_t doScanAll = pc.getInt("doScanAll") ;
  Int_t doScanNon = pc.getInt("doScanNon") ;

  // If scanning technique is not requested make integral-based cdf and return
  if (doScanNon) {
    return createIntRI(iset,nset) ;
  }
  if (doScanAll) {
    return createScanRI(iset,nset,numScanBins,intOrder) ;
  }
  if (doScanNum) {
    std::unique_ptr<RooAbsReal> tmp{createIntegral(iset)} ;
    Int_t isNum= !static_cast<RooRealIntegral&>(*tmp).numIntRealVars().empty();

    if (isNum) {
      coutI(NumIntegration) << "RooAbsPdf::createRunningIntegral(" << GetName() << ") integration over observable(s) " << iset << " involves numeric integration," << std::endl
             << "      constructing cdf though numeric integration of sampled pdf in " << numScanBins << " bins and applying order "
             << intOrder << " interpolation on integrated histogram." << std::endl
             << "      To override this choice of technique use argument ScanNone(), to change scan parameters use ScanParameters(nbins,order) argument" << std::endl ;
    }

    return isNum ? createScanRI(iset,nset,numScanBins,intOrder) : createIntRI(iset,nset) ;
  }
  return nullptr;
}



////////////////////////////////////////////////////////////////////////////////
/// Utility function for createRunningIntegral that construct an object
/// implementing the numeric scanning technique for calculating the running integral

RooFit::OwningPtr<RooAbsReal> RooAbsReal::createScanRI(const RooArgSet& iset, const RooArgSet& nset, Int_t numScanBins, Int_t intOrder)
{
  std::string name = std::string(GetName()) + "_NUMRUNINT_" + integralNameSuffix(iset,&nset).Data() ;
  RooRealVar* ivar = static_cast<RooRealVar*>(iset.first()) ;
  ivar->setBins(numScanBins,"numcdf") ;
  auto ret = std::make_unique<RooNumRunningInt>(name.c_str(),name.c_str(),*this,*ivar,"numrunint") ;
  ret->setInterpolationOrder(intOrder) ;
  return RooFit::makeOwningPtr<RooAbsReal>(std::move(ret));
}



////////////////////////////////////////////////////////////////////////////////
/// Utility function for createRunningIntegral. It creates an
/// object implementing the standard (analytical) integration
/// technique for calculating the running integral.

RooFit::OwningPtr<RooAbsReal> RooAbsReal::createIntRI(const RooArgSet& iset, const RooArgSet& nset)
{
  // Make list of input arguments keeping only RooRealVars
  RooArgList ilist ;
  for(RooAbsArg * arg : iset) {
    if (dynamic_cast<RooRealVar*>(arg)) {
      ilist.add(*arg) ;
    } else {
      coutW(InputArguments) << "RooAbsPdf::createRunningIntegral(" << GetName() << ") WARNING ignoring non-RooRealVar input argument " << arg->GetName() << std::endl ;
    }
  }

  RooArgList cloneList ;
  RooArgList loList ;
  RooArgSet clonedBranchNodes ;

  // Setup customizer that stores all cloned branches in our non-owning list
  RooCustomizer cust(*this,"cdf") ;
  cust.setCloneBranchSet(clonedBranchNodes) ;
  cust.setOwning(false) ;

  // Make integration observable x_prime for each observable x as well as an x_lowbound
  for(auto * rrv : static_range_cast<RooRealVar*>(ilist)) {

    // Make clone x_prime of each c.d.f observable x represening running integral
    RooRealVar* cloneArg = static_cast<RooRealVar*>(rrv->clone(Form("%s_prime",rrv->GetName()))) ;
    cloneList.add(*cloneArg) ;
    cust.replaceArg(*rrv,*cloneArg) ;

    // Make clone x_lowbound of each c.d.f observable representing low bound of x
    RooRealVar* cloneLo = static_cast<RooRealVar*>(rrv->clone(Form("%s_lowbound",rrv->GetName()))) ;
    cloneLo->setVal(rrv->getMin()) ;
    loList.add(*cloneLo) ;

    // Make parameterized binning from [x_lowbound,x] for each x_prime
    RooParamBinning pb(*cloneLo,*rrv,100) ;
    cloneArg->setBinning(pb,"CDF") ;

  }

  RooAbsReal* tmp = static_cast<RooAbsReal*>(cust.build()) ;

  // Construct final normalization set for c.d.f = integrated observables + any extra specified by user
  RooArgSet finalNset(nset) ;
  finalNset.add(cloneList,true) ;
  std::unique_ptr<RooAbsReal> cdf{tmp->createIntegral(cloneList,finalNset,"CDF")};

  // Transfer ownership of cloned items to top-level c.d.f object
  cdf->addOwnedComponents(*tmp) ;
  cdf->addOwnedComponents(cloneList) ;
  cdf->addOwnedComponents(loList) ;

  return RooFit::makeOwningPtr(std::move(cdf));
}


////////////////////////////////////////////////////////////////////////////////
/// Return a RooFunctor object bound to this RooAbsReal with given definition of observables
/// and parameters

RooFunctor* RooAbsReal::functor(const RooArgList& obs, const RooArgList& pars, const RooArgSet& nset) const
{
  RooArgSet realObs;
  getObservables(&obs, realObs);
  if (realObs.size() != obs.size()) {
    coutE(InputArguments) << "RooAbsReal::functor(" << GetName() << ") ERROR: one or more specified observables are not variables of this p.d.f" << std::endl ;
    return nullptr;
  }
  RooArgSet realPars;
  getObservables(&pars, realPars);
  if (realPars.size() != pars.size()) {
    coutE(InputArguments) << "RooAbsReal::functor(" << GetName() << ") ERROR: one or more specified parameters are not variables of this p.d.f" << std::endl ;
    return nullptr;
  }

  return new RooFunctor(*this,obs,pars,nset) ;
}



////////////////////////////////////////////////////////////////////////////////
/// Return a ROOT TF1,2,3 object bound to this RooAbsReal with given definition of observables
/// and parameters

TF1* RooAbsReal::asTF(const RooArgList& obs, const RooArgList& pars, const RooArgSet& nset) const
{
  // Check that specified input are indeed variables of this function
  RooArgSet realObs;
  getObservables(&obs, realObs) ;
  if (realObs.size() != obs.size()) {
    coutE(InputArguments) << "RooAbsReal::functor(" << GetName() << ") ERROR: one or more specified observables are not variables of this p.d.f" << std::endl ;
    return nullptr ;
  }
  RooArgSet realPars;
  getObservables(&pars, realPars) ;
  if (realPars.size() != pars.size()) {
    coutE(InputArguments) << "RooAbsReal::functor(" << GetName() << ") ERROR: one or more specified parameters are not variables of this p.d.f" << std::endl ;
    return nullptr ;
  }

  // Check that all obs and par are of type RooRealVar
  for (std::size_t i=0 ; i<obs.size() ; i++) {
    if (dynamic_cast<RooRealVar*>(obs.at(i))==nullptr) {
      coutE(ObjectHandling) << "RooAbsReal::asTF(" << GetName() << ") ERROR: proposed observable " << obs.at(0)->GetName() << " is not of type RooRealVar" << std::endl ;
      return nullptr ;
    }
  }
  for (std::size_t i=0 ; i<pars.size() ; i++) {
    if (dynamic_cast<RooRealVar*>(pars.at(i))==nullptr) {
      coutE(ObjectHandling) << "RooAbsReal::asTF(" << GetName() << ") ERROR: proposed parameter " << pars.at(0)->GetName() << " is not of type RooRealVar" << std::endl ;
      return nullptr ;
    }
  }

  // Create functor and TFx of matching dimension
  TF1* tf=nullptr ;
  RooFunctor* f ;
  switch(obs.size()) {
  case 1: {
    RooRealVar* x = static_cast<RooRealVar*>(obs.at(0)) ;
    f = functor(obs,pars,nset) ;
    tf = new TF1(GetName(),f,x->getMin(),x->getMax(),pars.size()) ;
    break ;
  }
  case 2: {
    RooRealVar* x = static_cast<RooRealVar*>(obs.at(0)) ;
    RooRealVar* y = static_cast<RooRealVar*>(obs.at(1)) ;
    f = functor(obs,pars,nset) ;
    tf = new TF2(GetName(),f,x->getMin(),x->getMax(),y->getMin(),y->getMax(),pars.size()) ;
    break ;
  }
  case 3: {
    RooRealVar* x = static_cast<RooRealVar*>(obs.at(0)) ;
    RooRealVar* y = static_cast<RooRealVar*>(obs.at(1)) ;
    RooRealVar* z = static_cast<RooRealVar*>(obs.at(2)) ;
    f = functor(obs,pars,nset) ;
    tf = new TF3(GetName(),f,x->getMin(),x->getMax(),y->getMin(),y->getMax(),z->getMin(),z->getMax(),pars.size()) ;
    break ;
  }
  default:
    coutE(InputArguments) << "RooAbsReal::asTF(" << GetName() << ") ERROR: " << obs.size()
           << " observables specified, but a ROOT TFx can only have  1,2 or 3 observables" << std::endl ;
    return nullptr ;
  }

  // Set initial parameter values of TFx to those of RooRealVars
  for (std::size_t i=0 ; i<pars.size() ; i++) {
    RooRealVar* p = static_cast<RooRealVar*>(pars.at(i)) ;
    tf->SetParameter(i,p->getVal()) ;
    tf->SetParName(i,p->GetName()) ;
    //tf->SetParLimits(i,p->getMin(),p->getMax()) ;
  }

  return tf ;
}


////////////////////////////////////////////////////////////////////////////////
/// Return function representing first, second or third order derivative of this function

RooDerivative* RooAbsReal::derivative(RooRealVar& obs, Int_t order, double eps)
{
  return derivative(obs, {}, order, eps);
}



////////////////////////////////////////////////////////////////////////////////
/// Return function representing first, second or third order derivative of this function

RooDerivative* RooAbsReal::derivative(RooRealVar& obs, const RooArgSet& normSet, Int_t order, double eps)
{
  std::string name=Form("%s_DERIV_%s",GetName(),obs.GetName()) ;
  std::string title=Form("Derivative of %s w.r.t %s ",GetName(),obs.GetName()) ;
  return new RooDerivative(name.c_str(),title.c_str(),*this,obs,normSet,order,eps) ;
}



////////////////////////////////////////////////////////////////////////////////
/// Return function representing moment of function of given order.
/// \param[in] obs Observable to calculate the moments for
/// \param[in] order Order of the moment
/// \param[in] central If true, the central moment is given by \f$ \langle (x- \langle x \rangle )^2 \rangle \f$
/// \param[in] takeRoot Calculate the square root

RooAbsMoment* RooAbsReal::moment(RooRealVar& obs, Int_t order, bool central, bool takeRoot)
{
  std::string name=Form("%s_MOMENT_%d%s_%s",GetName(),order,(central?"C":""),obs.GetName()) ;
  std::string title=Form("%sMoment of order %d of %s w.r.t %s ",(central?"Central ":""),order,GetName(),obs.GetName()) ;
  if (order==1) return new RooFirstMoment(name.c_str(),title.c_str(),*this,obs) ;
  if (order==2) return new RooSecondMoment(name.c_str(),title.c_str(),*this,obs,central,takeRoot) ;
  return new RooMoment(name.c_str(),title.c_str(),*this,obs,order,central,takeRoot) ;
}


////////////////////////////////////////////////////////////////////////////////
/// Return function representing moment of p.d.f (normalized w.r.t given observables) of given order.
/// \param[in] obs Observable to calculate the moments for
/// \param[in] normObs Normalise w.r.t. these observables
/// \param[in] order Order of the moment
/// \param[in] central If true, the central moment is given by \f$ \langle (x- \langle x \rangle )^2 \rangle \f$
/// \param[in] takeRoot Calculate the square root
/// \param[in] intNormObs If true, the moment of the function integrated over all normalization observables is returned.

RooAbsMoment* RooAbsReal::moment(RooRealVar& obs, const RooArgSet& normObs, Int_t order, bool central, bool takeRoot, bool intNormObs)
{
  std::string name=Form("%s_MOMENT_%d%s_%s",GetName(),order,(central?"C":""),obs.GetName()) ;
  std::string title=Form("%sMoment of order %d of %s w.r.t %s ",(central?"Central ":""),order,GetName(),obs.GetName()) ;

  if (order==1) return new RooFirstMoment(name.c_str(),title.c_str(),*this,obs,normObs,intNormObs) ;
  if (order==2) return new RooSecondMoment(name.c_str(),title.c_str(),*this,obs,normObs,central,takeRoot,intNormObs) ;
  return new RooMoment(name.c_str(),title.c_str(),*this,obs,normObs,order,central,takeRoot,intNormObs) ;
}



////////////////////////////////////////////////////////////////////////////////
///
/// Return value of x (in range xmin,xmax) at which function equals yval.
/// (Calculation is performed with Brent root finding algorithm)

double RooAbsReal::findRoot(RooRealVar& x, double xmin, double xmax, double yval)
{
  double result(0) ;
  RooBrentRootFinder(RooRealBinding(*this,x)).findRoot(result,xmin,xmax,yval) ;
  return result ;
}



////////////////////////////////////////////////////////////////////////////////
/// Perform a \f$ \chi^2 \f$ fit to given histogram. By default the fit is executed through the MINUIT
/// commands MIGRAD, HESSE in succession
///
/// The following named arguments are supported
///
/// <table>
/// <tr><th> <th> Options to control construction of chi2
/// <tr><td> `Extended(bool flag)` <td> **Only applicable when fitting a RooAbsPdf**. Scale the normalized pdf by the number of events predicted by the model instead of scaling by the total data weight.
///                                     This imposes a constraint on the predicted number of events analogous to the extended term in a likelihood fit.
///                                     - If you don't pass this command, an extended fit will be done by default if the pdf makes a prediction on the number of events
///                                       (in RooFit jargon, "if the pdf can be extended").
///                                     - Passing `Extended(true)` when the the pdf makes no prediction on the expected number of events will result in error messages,
///                                       and the chi2 will fall back to the total data weight to scale the normalized pdf.
///                                     - There are cases where the fit **must** be done in extended mode. This happens for example when you have a RooAddPdf
///                                       where the coefficients represent component yields. If the fit is not extended, these coefficients will not be
///                                       well-defined, as the RooAddPdf always normalizes itself. If you pass `Extended(false)` in such a case, an error will be
///                                       printed and you'll most likely get garbage results.
/// <tr><td> `Range(const char* name)`         <td> Fit only data inside range with given name
/// <tr><td> `Range(double lo, double hi)` <td> Fit only data inside given range. A range named "fit" is created on the fly on all observables.
///                                               Multiple comma separated range names can be specified.
/// <tr><td> `NumCPU(int num)`                 <td> Parallelize NLL calculation on num CPUs
/// <tr><td> `Optimize(bool flag)`           <td> Activate constant term optimization (on by default)
/// <tr><td> `IntegrateBins()`                 <td> Integrate PDF within each bin. This sets the desired precision.
///
/// <tr><th> <th> Options to control flow of fit procedure
/// <tr><td> `InitialHesse(bool flag)`      <td> Flag controls if HESSE before MIGRAD as well, off by default
/// <tr><td> `Hesse(bool flag)`             <td> Flag controls if HESSE is run after MIGRAD, on by default
/// <tr><td> `Minos(bool flag)`             <td> Flag controls if MINOS is run after HESSE, on by default
/// <tr><td> `Minos(const RooArgSet& set)`    <td> Only run MINOS on given subset of arguments
/// <tr><td> `Save(bool flag)`              <td> Flag controls if RooFitResult object is produced and returned, off by default
/// <tr><td> `Strategy(Int_t flag)`           <td> Set Minuit strategy (0 through 2, default is 1)
///
/// <tr><th> <th> Options to control informational output
/// <tr><td> `Verbose(bool flag)`           <td> Flag controls if verbose output is printed (NLL, parameter changes during fit
/// <tr><td> `Timer(bool flag)`             <td> Time CPU and wall clock consumption of fit steps, off by default
/// <tr><td> `PrintLevel(Int_t level)`        <td> Set Minuit print level (-1 through 3, default is 1). At -1 all RooFit informational
///                                              messages are suppressed as well
/// <tr><td> `Warnings(bool flag)`          <td> Enable or disable MINUIT warnings (enabled by default)
/// <tr><td> `PrintEvalErrors(Int_t numErr)`  <td> Control number of p.d.f evaluation errors printed per likelihood evaluation. A negative
///                                              value suppress output completely, a zero value will only print the error count per p.d.f component,
///                                              a positive value is will print details of each error up to numErr messages per p.d.f component.
/// </table>
///

RooFit::OwningPtr<RooFitResult> RooAbsReal::chi2FitTo(RooDataHist& data, const RooCmdArg& arg1,  const RooCmdArg& arg2,
                const RooCmdArg& arg3,  const RooCmdArg& arg4, const RooCmdArg& arg5,
                const RooCmdArg& arg6,  const RooCmdArg& arg7, const RooCmdArg& arg8)
{
  CREATE_CMD_LIST;
  return chi2FitTo(data,l) ;
}



////////////////////////////////////////////////////////////////////////////////
/// Calls RooAbsReal::createChi2(RooDataSet& data, const RooLinkedList& cmdList) and returns fit result.
///
/// List of possible commands in the `cmdList`:
///
///  <table>
///  <tr><th> Type of CmdArg    <th>    Effect on \f$ \chi^2 \f$
///  <tr><td>
///  <tr><td> `DataError()`  <td>  Choose between:
///  - RooAbsData::Expected: Expected Poisson error (\f$ \sqrt{n_\text{expected}} \f$ from the PDF).
///  - RooAbsData::SumW2: The observed error from the square root of the sum of weights,
///    i.e., symmetric errors calculated with the standard deviation of a Poisson distribution.
///  - RooAbsData::Poisson: Asymmetric errors from the central 68 % interval around a Poisson distribution with mean \f$ n_\text{observed} \f$.
///    If for a given bin \f$ n_\text{expected} \f$ is lower than the \f$ n_\text{observed} \f$, the lower uncertainty is taken
///    (e.g., the difference between the mean and the 16 % quantile).
///    If \f$ n_\text{expected} \f$ is higher than \f$ n_\text{observed} \f$, the higher uncertainty is taken
///    (e.g., the difference between the 84 % quantile and the mean).
///  - RooAbsData::Auto (default): RooAbsData::Expected for unweighted data, RooAbsData::SumW2 for weighted data.
///  <tr><td>
///  `Extended()` <td>  Use expected number of events of an extended p.d.f as normalization
///  <tr><td>
///  NumCPU()     <td> Activate parallel processing feature
///  <tr><td>
///  Range()      <td> Calculate \f$ \chi^2 \f$ only in selected region
///  <tr><td>
///  Verbose()    <td> Verbose output of GOF framework
///  <tr><td>
///  IntegrateBins()  <td> Integrate PDF within each bin. This sets the desired precision. Only useful for binned fits.
/// <tr><td> `SumCoefRange()` <td>  Set the range in which to interpret the coefficients of RooAddPdf components
/// <tr><td> `SplitRange()`   <td>  Fit ranges used in different categories get named after the category.
/// Using `Range("range"), SplitRange()` as switches, different ranges could be set like this:
/// ```
/// myVariable.setRange("range_pi0", 135, 210);
/// myVariable.setRange("range_gamma", 50, 210);
/// ```
/// <tr><td> `ConditionalObservables(Args_t &&... argsOrArgSet)`  <td>  Define projected observables.
///                                Arguments can either be multiple RooRealVar or a single RooArgSet containing them.
///
/// </table>

RooFit::OwningPtr<RooFitResult> RooAbsReal::chi2FitTo(RooDataHist &data, const RooLinkedList &cmdList)
{
   return RooFit::makeOwningPtr(RooFit::FitHelpers::fitTo(*this, data, cmdList, true));
}




////////////////////////////////////////////////////////////////////////////////
/// Create a \f$ \chi^2 \f$ variable from a histogram and this function.
///
/// \param arg1,arg2,arg3,arg4,arg5,arg6,arg7,arg8 ordered arguments
///
/// The list of supported command arguments is given in the documentation for
///     RooChi2Var::RooChi2Var(const char *name, const char* title, RooAbsReal& func, RooDataHist& hdata, const RooCmdArg&,const RooCmdArg&,const RooCmdArg&, const RooCmdArg&,const RooCmdArg&,const RooCmdArg&, const RooCmdArg&,const RooCmdArg&,const RooCmdArg&).
///
/// \param data Histogram with data
/// \return \f$ \chi^2 \f$ variable

RooFit::OwningPtr<RooAbsReal> RooAbsReal::createChi2(RooDataHist &data, const RooCmdArg &arg1, const RooCmdArg &arg2,
                                                     const RooCmdArg &arg3, const RooCmdArg &arg4,
                                                     const RooCmdArg &arg5, const RooCmdArg &arg6,
                                                     const RooCmdArg &arg7, const RooCmdArg &arg8)
{
   CREATE_CMD_LIST;
   return createChi2(data, l);
}

////////////////////////////////////////////////////////////////////////////////
/// \see RooAbsReal::createChi2(RooDataHist&,const RooCmdArg&,const RooCmdArg&,const RooCmdArg&,const RooCmdArg&,const RooCmdArg&,const RooCmdArg&,const RooCmdArg&,const RooCmdArg&)
/// \param data hist data
/// \param cmdList List with RooCmdArg() from the table

RooFit::OwningPtr<RooAbsReal> RooAbsReal::createChi2(RooDataHist& data, const RooLinkedList& cmdList)
{
   return RooFit::makeOwningPtr(RooFit::FitHelpers::createChi2(*this, data, cmdList));
}

////////////////////////////////////////////////////////////////////////////////
/// Perform a 2-D \f$ \chi^2 \f$ fit using a series of x and y values stored in the dataset `xydata`.
/// The y values can either be the event weights, or can be another column designated
/// by the YVar() argument. The y value must have errors defined for the \f$ \chi^2 \f$ to
/// be well defined.
///
/// <table>
/// <tr><th><th> Options to control construction of the chi-square
/// <tr><td> `YVar(RooRealVar& yvar)`          <td>  Designate given column in dataset as Y value
/// <tr><td> `Integrate(bool flag)`          <td>  Integrate function over range specified by X errors
///                                    rather than take value at bin center.
///
/// <tr><th><th> Options to control flow of fit procedure
/// <tr><td> `InitialHesse(bool flag)`      <td>  Flag controls if HESSE before MIGRAD as well, off by default
/// <tr><td> `Hesse(bool flag)`             <td>  Flag controls if HESSE is run after MIGRAD, on by default
/// <tr><td> `Minos(bool flag)`             <td>  Flag controls if MINOS is run after HESSE, on by default
/// <tr><td> `Minos(const RooArgSet& set)`    <td>  Only run MINOS on given subset of arguments
/// <tr><td> `Save(bool flag)`              <td>  Flag controls if RooFitResult object is produced and returned, off by default
/// <tr><td> `Strategy(Int_t flag)`           <td>  Set Minuit strategy (0 through 2, default is 1)
///
/// <tr><th><th> Options to control informational output
/// <tr><td> `Verbose(bool flag)`           <td>  Flag controls if verbose output is printed (NLL, parameter changes during fit
/// <tr><td> `Timer(bool flag)`             <td>  Time CPU and wall clock consumption of fit steps, off by default
/// <tr><td> `PrintLevel(Int_t level)`        <td>  Set Minuit print level (-1 through 3, default is 1). At -1 all RooFit informational
///                                   messages are suppressed as well
/// <tr><td> `Warnings(bool flag)`          <td>  Enable or disable MINUIT warnings (enabled by default)
/// <tr><td> `PrintEvalErrors(Int_t numErr)`  <td>  Control number of p.d.f evaluation errors printed per likelihood evaluation. A negative
///                                   value suppress output completely, a zero value will only print the error count per p.d.f component,
///                                   a positive value is will print details of each error up to numErr messages per p.d.f component.
/// </table>

RooFit::OwningPtr<RooFitResult> RooAbsReal::chi2FitTo(RooDataSet& xydata, const RooCmdArg& arg1,  const RooCmdArg& arg2,
                  const RooCmdArg& arg3,  const RooCmdArg& arg4, const RooCmdArg& arg5,
                  const RooCmdArg& arg6,  const RooCmdArg& arg7, const RooCmdArg& arg8)
{
  CREATE_CMD_LIST;
  return chi2FitTo(xydata,l) ;
}




////////////////////////////////////////////////////////////////////////////////
/// \copydoc RooAbsReal::chi2FitTo(RooDataSet&,const RooCmdArg&,const RooCmdArg&,const RooCmdArg&,const RooCmdArg&,const RooCmdArg&,const RooCmdArg&,const RooCmdArg&,const RooCmdArg&)

RooFit::OwningPtr<RooFitResult> RooAbsReal::chi2FitTo(RooDataSet &xydata, const RooLinkedList &cmdList)
{
   return RooFit::makeOwningPtr(RooFit::FitHelpers::fitTo(*this, xydata, cmdList, true));
}




////////////////////////////////////////////////////////////////////////////////
/// Create a \f$ \chi^2 \f$ from a series of x and y values stored in a dataset.
/// The y values can either be the event weights (default), or can be another column designated
/// by the YVar() argument. The y value must have errors defined for the \f$ \chi^2 \f$ to
/// be well defined.
///
/// The following named arguments are supported
///
/// | | Options to control construction of the \f$ \chi^2 \f$
/// |-|-----------------------------------------
/// | `YVar(RooRealVar& yvar)`  | Designate given column in dataset as Y value
/// | `Integrate(bool flag)`  | Integrate function over range specified by X errors rather than take value at bin center.
///

RooFit::OwningPtr<RooAbsReal> RooAbsReal::createChi2(RooDataSet& data, const RooCmdArg& arg1,  const RooCmdArg& arg2,
                 const RooCmdArg& arg3,  const RooCmdArg& arg4, const RooCmdArg& arg5,
                 const RooCmdArg& arg6,  const RooCmdArg& arg7, const RooCmdArg& arg8)
{
  CREATE_CMD_LIST;
  return createChi2(data,l) ;
}


////////////////////////////////////////////////////////////////////////////////
/// See RooAbsReal::createChi2(RooDataSet&,const RooCmdArg&,const RooCmdArg&,const RooCmdArg&,const RooCmdArg&,const RooCmdArg&,const RooCmdArg&,const RooCmdArg&,const RooCmdArg&)

RooFit::OwningPtr<RooAbsReal> RooAbsReal::createChi2(RooDataSet &data, const RooLinkedList &cmdList)
{
   return RooFit::makeOwningPtr(RooFit::FitHelpers::createChi2(*this, data, cmdList));
}



////////////////////////////////////////////////////////////////////////////////
/// Return current evaluation error logging mode.

RooAbsReal::ErrorLoggingMode RooAbsReal::evalErrorLoggingMode()
{
  return evalErrorData().mode  ;
}

////////////////////////////////////////////////////////////////////////////////
/// Set evaluation error logging mode. Options are
///
/// PrintErrors - Print each error through RooMsgService() as it occurs
/// CollectErrors - Accumulate errors, but do not print them. A subsequent call
///                 to printEvalErrors() will print a summary
/// CountErrors - Accumulate error count, but do not print them.
///

void RooAbsReal::setEvalErrorLoggingMode(RooAbsReal::ErrorLoggingMode m)
{
  evalErrorData().mode = m;
}


////////////////////////////////////////////////////////////////////////////////

void RooAbsReal::setParameterizeIntegral(const RooArgSet& paramVars)
{
  std::string plist ;
  for (auto const* arg : paramVars) {
    if (!dependsOnValue(*arg)) {
      coutW(InputArguments) << "RooAbsReal::setParameterizeIntegral(" << GetName()
             << ") function does not depend on listed parameter " << arg->GetName() << ", ignoring" << std::endl ;
      continue ;
    }
    if (!plist.empty()) plist += ":" ;
    plist += arg->GetName() ;
  }
  setStringAttribute("CACHEPARAMINT",plist.c_str()) ;
}


/** Base function for computing multiple values of a RooAbsReal.
\param ctx An evaluation context object
**/
void RooAbsReal::doEval(RooFit::EvalContext & ctx) const
{
  std::span<double> output = ctx.output();

  // Find all servers that are serving real numbers to us, retrieve their batch data,
  // and switch them into "always clean" operating mode, so they return always the last-set value.
  struct ServerData {
    RooAbsArg* server;
    std::span<const double> batch;
    double oldValue;
    RooAbsArg::OperMode oldOperMode;
    bool oldValueDirty;
    bool oldShapeDirty;
  };
  std::vector<ServerData> ourServers;
  ourServers.reserve(servers().size());

  for (auto server : servers()) {
    auto serverValues = ctx.at(server);
    if(serverValues.empty()) continue;

    // maybe we are still missing inhibit dirty here
    auto oldOperMode = server->operMode();
    // See note at the bottom of this function to learn why we can only set
    // the operation mode to "always clean" if there are no other value
    // clients.
    server->setOperMode(RooAbsArg::AClean);
    ourServers.push_back({server,
        serverValues,
        server->isCategory() ? static_cast<RooAbsCategory const*>(server)->getCurrentIndex() : static_cast<RooAbsReal const*>(server)->_value,
        oldOperMode,
        server->_valueDirty,
        server->_shapeDirty});
    // Prevent the server from evaluating; just return cached result, which we will side load:
  }


  // Make sure that we restore all state when we finish:
  struct RestoreStateRAII {
    RestoreStateRAII(std::vector<ServerData>& servers) :
      _servers{servers} { }

    ~RestoreStateRAII() {
      for (auto& serverData : _servers) {
        serverData.server->setCachedValue(serverData.oldValue, true);
        serverData.server->setOperMode(serverData.oldOperMode);
        serverData.server->_valueDirty = serverData.oldValueDirty;
        serverData.server->_shapeDirty = serverData.oldShapeDirty;
      }
    }

    std::vector<ServerData>& _servers;
  } restoreState{ourServers};


  // Advising to implement the batch interface makes only sense if the batch was not a scalar.
  // Otherwise, there would be no speedup benefit.
  if(output.size() > 1 && RooMsgService::instance().isActive(this, RooFit::FastEvaluations, RooFit::INFO)) {
    coutI(FastEvaluations) << "The class " << ClassName() << " does not implement the faster batch evaluation interface."
        << " Consider requesting or implementing it to benefit from a speed up." << std::endl;
  }


  // For each event, write temporary values into our servers' caches, and run a single-value computation.

  for (std::size_t i=0; i < output.size(); ++i) {
    for (auto& serv : ourServers) {
      serv.server->setCachedValue(serv.batch[std::min(i, serv.batch.size()-1)], false);
    }

    output[i] = evaluate();
  }
}

double RooAbsReal::_DEBUG_getVal(const RooArgSet* normalisationSet) const {

  const bool tmpFast = _fast;
  const double tmp = _value;

  double fullEval = 0.;
  try {
    fullEval = getValV(normalisationSet);
  }
  catch (CachingError& error) {
    throw CachingError(std::move(error),
        FormatPdfTree() << *this);
  }

  const double ret = (_fast && !_inhibitDirty) ? _value : fullEval;

  if (std::isfinite(ret) && ( ret != 0. ? (ret - fullEval)/ret : ret - fullEval) > 1.E-9) {
#ifndef NDEBUG
    gSystem->StackTrace();
#endif
    FormatPdfTree formatter;
    formatter << "--> (Scalar computation wrong here:)\n"
            << GetName() << " " << this << " _fast=" << tmpFast
            << "\n\tcached _value=" << std::setprecision(16) << tmp
            << "\n\treturning    =" << ret
            << "\n\trecomputed   =" << fullEval
            << "\n\tnew _value   =" << _value << "] ";
    formatter << "\nServers:";
    for (const auto server : _serverList) {
      formatter << "\n  ";
      server->printStream(formatter.stream(), kName | kClassName | kArgs | kExtras | kAddress | kValue, kInline);
    }

    throw CachingError(formatter);
  }

  return ret;
}


bool RooAbsReal::redirectServersHook(const RooAbsCollection & newServerList, bool mustReplaceAll,
                                    bool nameChange, bool isRecursiveStep)
{
  _lastNormSetId = RooFit::UniqueId<RooArgSet>::nullval;
  return RooAbsArg::redirectServersHook(newServerList, mustReplaceAll, nameChange, isRecursiveStep);
}


////////////////////////////////////////////////////////////////////////////////

void RooAbsReal::enableOffsetting(bool flag)
{
  for (RooAbsArg* arg : servers()) {
    if(auto realArg = dynamic_cast<RooAbsReal*>(arg)) {
      realArg->enableOffsetting(flag) ;
    }
  }
}


RooAbsReal::Ref::Ref(double val) : _ref{RooFit::RooConst(val)} {}

////////////////////////////////////////////////////////////////////////////////
/// Calling RooAbsReal::getVal() with an r-value reference is a common
/// performance trap, because this usually happens when implicitly constructing
/// the RooArgSet to be used as the parameter (for example, in calls like
/// `pdf.getVal(x)`).
///
/// Creating the RooArgSet can cause quite some overhead, especially when the
/// evaluated object is just a simple variable. Even worse, many RooFit objects
/// internally cache information using the uniqueId() of the normalization set
/// as the key. So by constructing normalization sets in place, RooFits caching
/// logic is broken.
///
/// To avoid these kind of problems, getVal() will just throw an error when
/// it's called with an r-value reference. This also catches the cases where
/// one uses it in Python, implicitly creating the normalization set from a
/// Python list or set.
double RooAbsReal::getVal(RooArgSet &&) const
{
   std::stringstream errMsg;
   errMsg << "calling RooAbsReal::getVal() with r-value references to the normalization set is not allowed, because "
             "it breaks RooFits caching logic and potentially introduces significant overhead. Please explicitly "
             "create the RooArgSet outside the call to getVal().";
   coutF(Eval) << errMsg.str() << std::endl;
   throw std::runtime_error(errMsg.str());
}
