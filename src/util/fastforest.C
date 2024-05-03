/**
MIT License
Copyright (c) 2020 Jonas Rembser
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "fastforest.h"

#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <unordered_map>

using namespace fastforest;


void fastforest::detail::correctIndices(std::vector<int>::iterator begin,
                                        std::vector<int>::iterator end,
                                        fastforest::detail::IndexMap const& nodeIndices,
                                        fastforest::detail::IndexMap const& leafIndices) {
  for (auto it = begin; it != end; ++it) {
    if (nodeIndices.count(*it)) {
      *it = nodeIndices.at(*it);
    } else if (leafIndices.count(*it)) {
      *it = -leafIndices.at(*it);
    } else {
      throw std::runtime_error("something is wrong in the node structure");
    }
  }
}

namespace {
    namespace util {

        inline bool isInteger(const std::string& s) {
          if (s.empty() || ((!isdigit(s[0])) && (s[0] != '-') && (s[0] != '+')))
            return false;

          char* p;
          strtol(s.c_str(), &p, 10);

          return (*p == 0);
        }

        template <class NumericType>
        struct NumericAfterSubstrOutput {
            explicit NumericAfterSubstrOutput() : value{0}, found{false}, failed{true} {}
            NumericType value;
            bool found;
            bool failed;
            std::string rest;
        };

        template <class NumericType>
        inline NumericAfterSubstrOutput<NumericType> numericAfterSubstr(std::string const& str,
                                                                        std::string const& substr) {
          std::string rest;
          NumericAfterSubstrOutput<NumericType> output;
          output.rest = str;

          auto found = str.find(substr);
          if (found != std::string::npos) {
            output.found = true;
            std::stringstream ss(str.substr(found + substr.size(), str.size() - found + substr.size()));
            ss >> output.value;
            if (!ss.fail()) {
              output.failed = false;
              output.rest = ss.str();
            }
          }
          return output;
        }

        std::vector<std::string> split(std::string const& strToSplit, char delimeter) {
          std::stringstream ss(strToSplit);
          std::string item;
          std::vector<std::string> splittedStrings;
          while (std::getline(ss, item, delimeter)) {
            splittedStrings.push_back(item);
          }
          return splittedStrings;
        }

        bool exists(std::string const& filename) {
          if (FILE* file = fopen(filename.c_str(), "r")) {
            fclose(file);
            return true;
          } else {
            return false;
          }
        }

    }  // namespace util

    void terminateTree(fastforest::FastForest& ff,
                       int& nPreviousNodes,
                       int& nPreviousLeaves,
                       fastforest::detail::IndexMap& nodeIndices,
                       fastforest::detail::IndexMap& leafIndices,
                       int& treesSkipped) {
      using namespace fastforest::detail;
      correctIndices(ff.rightIndices_.begin() + nPreviousNodes, ff.rightIndices_.end(), nodeIndices, leafIndices);
      correctIndices(ff.leftIndices_.begin() + nPreviousNodes, ff.leftIndices_.end(), nodeIndices, leafIndices);

      if (nPreviousNodes != ff.cutValues_.size()) {
        ff.treeNumbers_.push_back(ff.rootIndices_.size() + treesSkipped);
        ff.rootIndices_.push_back(nPreviousNodes);
      } else {
        int treeNumbers = ff.rootIndices_.size() + treesSkipped;
        ++treesSkipped;
        ff.baseResponses_[treeNumbers % ff.baseResponses_.size()] += ff.responses_.back();
        ff.responses_.pop_back();
      }

      nodeIndices.clear();
      leafIndices.clear();
      nPreviousNodes = ff.cutValues_.size();
      nPreviousLeaves = ff.responses_.size();
    }

}  // namespace


void fastforest::details::softmaxTransformInplace(TreeEnsembleResponseType* out, int nOut) {
  // Do softmax transformation inplace, mimicing exactly the Softmax function
  // in the src/common/math.h source file of xgboost.
  double norm = 0.;
  TreeEnsembleResponseType wmax = *out;
  int i = 1;
  for (; i < nOut; ++i) {
    wmax = std::max(out[i], wmax);
  }
  i = 0;
  for (; i < nOut; ++i) {
    auto& x = out[i];
    x = std::exp(x - wmax);
    norm += x;
  }
  i = 0;
  for (; i < nOut; ++i) {
    out[i] /= static_cast<double>(norm);
  }
}

std::vector<TreeEnsembleResponseType> fastforest::FastForest::softmax(const FeatureType* array,
                                                                      TreeEnsembleResponseType baseResponse) const {
  auto out = std::vector<TreeEnsembleResponseType>(nClasses());
  softmax(array, out.data(), baseResponse);
  return out;
}

void fastforest::FastForest::softmax(const FeatureType* array,
                                     TreeEnsembleResponseType* out,
                                     TreeEnsembleResponseType baseResponse) const {
  int nClass = nClasses();
  if (nClass <= 2) {
    throw std::runtime_error(
            "Error in FastForest::softmax : binary classification models don't support softmax evaluation. Plase set "
            "the number of classes in the FastForest-creating function if this is a multiclassification model.");
  }

  evaluate(array, out, nClass, baseResponse);
  fastforest::details::softmaxTransformInplace(out, nClass);
}

void fastforest::FastForest::evaluate(const FeatureType* array,
                                      TreeEnsembleResponseType* out,
                                      int nOut,
                                      TreeEnsembleResponseType baseResponse) const {
  for (int i = 0; i < nOut; ++i) {
    out[i] = baseResponse + baseResponses_[i];
  }

  int iRootIndex = 0;
  for (int index : rootIndices_) {
    do {
      auto r = rightIndices_[index];
      auto l = leftIndices_[index];
      index = array[cutIndices_[index]] > cutValues_[index] ? r : l;
    } while (index > 0);
    out[treeNumbers_[iRootIndex] % nOut] += responses_[-index];
    ++iRootIndex;
  }
}

TreeEnsembleResponseType fastforest::FastForest::evaluateBinary(const FeatureType* array,
                                                                TreeEnsembleResponseType baseResponse) const {
  TreeEnsembleResponseType out{baseResponse + baseResponses_[0]};

  for (int index : rootIndices_) {
    do {
      auto r = rightIndices_[index];
      auto l = leftIndices_[index];
      index = array[cutIndices_[index]] > cutValues_[index] ? r : l;
    } while (index > 0);
    out += responses_[-index];
  }

  return out;
}

FastForest fastforest::load_bin(std::string const& txtpath) {
  std::ifstream ifs(txtpath, std::ios::binary);
  return load_bin(ifs);
}

FastForest fastforest::load_bin(std::istream& is) {
  FastForest ff;

  int nRootNodes;
  int nNodes;
  int nLeaves;

  is.read((char*)&nRootNodes, sizeof(int));
  is.read((char*)&nNodes, sizeof(int));
  is.read((char*)&nLeaves, sizeof(int));

  ff.rootIndices_.resize(nRootNodes);
  ff.cutIndices_.resize(nNodes);
  ff.cutValues_.resize(nNodes);
  ff.leftIndices_.resize(nNodes);
  ff.rightIndices_.resize(nNodes);
  ff.responses_.resize(nLeaves);
  ff.treeNumbers_.resize(nRootNodes);

  is.read((char*)ff.rootIndices_.data(), nRootNodes * sizeof(int));
  is.read((char*)ff.cutIndices_.data(), nNodes * sizeof(CutIndexType));
  is.read((char*)ff.cutValues_.data(), nNodes * sizeof(FeatureType));
  is.read((char*)ff.leftIndices_.data(), nNodes * sizeof(int));
  is.read((char*)ff.rightIndices_.data(), nNodes * sizeof(int));
  is.read((char*)ff.responses_.data(), nLeaves * sizeof(TreeResponseType));
  is.read((char*)ff.treeNumbers_.data(), nRootNodes * sizeof(int));

  int nBaseResponses;
  is.read((char*)&nBaseResponses, sizeof(int));
  ff.baseResponses_.resize(nBaseResponses);
  is.read((char*)ff.baseResponses_.data(), nBaseResponses * sizeof(TreeEnsembleResponseType));

  return ff;
}

void fastforest::FastForest::write_bin(std::string const& filename) const {
  std::ofstream os(filename, std::ios::binary);

  int nRootNodes = rootIndices_.size();
  int nNodes = cutValues_.size();
  int nLeaves = responses_.size();
  int nBaseResponses = baseResponses_.size();

  os.write((const char*)&nRootNodes, sizeof(int));
  os.write((const char*)&nNodes, sizeof(int));
  os.write((const char*)&nLeaves, sizeof(int));

  os.write((const char*)rootIndices_.data(), nRootNodes * sizeof(int));
  os.write((const char*)cutIndices_.data(), nNodes * sizeof(CutIndexType));
  os.write((const char*)cutValues_.data(), nNodes * sizeof(FeatureType));
  os.write((const char*)leftIndices_.data(), nNodes * sizeof(int));
  os.write((const char*)rightIndices_.data(), nNodes * sizeof(int));
  os.write((const char*)responses_.data(), nLeaves * sizeof(TreeResponseType));
  os.write((const char*)treeNumbers_.data(), nRootNodes * sizeof(int));

  os.write((const char*)&nBaseResponses, sizeof(int));
  os.write((const char*)baseResponses_.data(), nBaseResponses * sizeof(TreeEnsembleResponseType));
  os.close();
}

FastForest fastforest::load_txt(std::string const& txtpath, std::vector<std::string>& features, int nClasses) {
  const std::string info = "constructing FastForest from " + txtpath + ": ";

  if (!util::exists(txtpath)) {
    throw std::runtime_error(info + "file does not exists");
  }

  std::ifstream file(txtpath);
  return load_txt(file, features, nClasses);
}

FastForest fastforest::load_txt(std::istream& file, std::vector<std::string>& features, int nClasses) {
  if (nClasses < 2) {
    throw std::runtime_error("Error in fastforest::load_txt : nClasses has to be at least two");
  }

  const std::string info = "constructing FastForest from istream: ";

  FastForest ff;
  ff.baseResponses_.resize(nClasses == 2 ? 1 : nClasses);

  int treesSkipped = 0;

  int nVariables = 0;
  std::unordered_map<std::string, int> varIndices;
  bool fixFeatures = false;

  if (!features.empty()) {
    fixFeatures = true;
    nVariables = features.size();
    for (int i = 0; i < nVariables; ++i) {
      varIndices[features[i]] = i;
    }
  }

  std::string line;

  fastforest::detail::IndexMap nodeIndices;
  fastforest::detail::IndexMap leafIndices;

  int nPreviousNodes = 0;
  int nPreviousLeaves = 0;

  while (std::getline(file, line)) {
    auto foundBegin = line.find("[");
    auto foundEnd = line.find("]");
    if (foundBegin != std::string::npos) {
      auto subline = line.substr(foundBegin + 1, foundEnd - foundBegin - 1);
      if (util::isInteger(subline) && !ff.responses_.empty()) {
        terminateTree(ff, nPreviousNodes, nPreviousLeaves, nodeIndices, leafIndices, treesSkipped);
      } else if (!util::isInteger(subline)) {
        std::stringstream ss(line);
        int index;
        ss >> index;
        line = ss.str();

        auto splitstring = util::split(subline, '<');
        auto const& varName = splitstring[0];
        FeatureType cutValue = std::stold(splitstring[1]);
        if (!varIndices.count(varName)) {
          if (fixFeatures) {
            throw std::runtime_error(info + "feature " + varName + " not in list of features");
          }
          varIndices[varName] = nVariables;
          features.push_back(varName);
          ++nVariables;
        }
        int yes;
        int no;
        auto output = util::numericAfterSubstr<int>(line, "yes=");
        if (!output.failed) {
          yes = output.value;
        } else {
          throw std::runtime_error(info + "problem while parsing the text dump");
        }
        output = util::numericAfterSubstr<int>(output.rest, "no=");
        if (!output.failed) {
          no = output.value;
        } else {
          throw std::runtime_error(info + "problem while parsing the text dump");
        }

        ff.cutValues_.push_back(cutValue);
        ff.cutIndices_.push_back(varIndices[varName]);
        ff.leftIndices_.push_back(yes);
        ff.rightIndices_.push_back(no);
        auto nNodeIndices = nodeIndices.size();
        nodeIndices[index] = nNodeIndices + nPreviousNodes;
      }

    } else {
      auto output = util::numericAfterSubstr<TreeResponseType>(line, "leaf=");
      if (output.found) {
        std::stringstream ss(line);
        int index;
        ss >> index;
        line = ss.str();

        ff.responses_.push_back(output.value);
        auto nLeafIndices = leafIndices.size();
        leafIndices[index] = nLeafIndices + nPreviousLeaves;
      }
    }
  }
  terminateTree(ff, nPreviousNodes, nPreviousLeaves, nodeIndices, leafIndices, treesSkipped);

  if (nClasses > 2 && (ff.rootIndices_.size() + treesSkipped) % nClasses != 0) {
    throw std::runtime_error(std::string{"Error in FastForest construction : Forest has "} +
                             std::to_string(ff.rootIndices_.size()) + " trees, " + "which is not compatible with " +
                             std::to_string(nClasses) + " classes!");
  }

  return ff;
}