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

#ifndef FastForest_h
#define FastForest_h

#include <array>
#include <cmath>
#include <istream>
#include <string>
#include <vector>
#include <unordered_map>

namespace fastforest {

    // The double number type that will be used to accept features and store cut values
    typedef double FeatureType;
    // Tue double number type that the individual trees return their responses in
    typedef double TreeResponseType;
    // The double number type that is used to sum the individual tree responses
    typedef double TreeEnsembleResponseType;
    // This integer type stores the indices of the feature employed in each cut.
    // Set to `unsigned char` for most compact fastforest ofjects if you have less than 256 features.
    typedef unsigned int CutIndexType;

    namespace detail {

        typedef std::unordered_map<int, int> IndexMap;

        void correctIndices(std::vector<int>::iterator begin,
                            std::vector<int>::iterator end,
                            IndexMap const& nodeIndices,
                            IndexMap const& leafIndices);

    }

    // The base response you have to use with older XGBoost versions might be
    // zero, so try to explicitely pass zero to the model evaluation if the
    // results from this library are incorrect.
    const TreeEnsembleResponseType defaultBaseResponse = 0.5;

    namespace details {

        void softmaxTransformInplace(TreeEnsembleResponseType* out, int nOut);

    }

    struct FastForest {
        inline TreeEnsembleResponseType operator()(const FeatureType* array,
                                                   TreeEnsembleResponseType baseResponse = defaultBaseResponse) const {
          return evaluateBinary(array, baseResponse);
        }

        template <int nClasses>
        std::array<TreeEnsembleResponseType, nClasses> softmax(
                const FeatureType* array, TreeEnsembleResponseType baseResponse = defaultBaseResponse) const {
          // static softmax interface: no manual memory allocation, but requires to know nClasses at compile time
          static_assert(nClasses >= 3, "nClasses should be >= 3");
          std::array<TreeEnsembleResponseType, nClasses> out{};
          evaluate(array, out.data(), nClasses, baseResponse);
          details::softmaxTransformInplace(out.data(), nClasses);
          return out;
        }

        // dynamic softmax interface with manually allocated std::vector: simple but inefficient
        std::vector<TreeEnsembleResponseType> softmax(
                const FeatureType* array, TreeEnsembleResponseType baseResponse = defaultBaseResponse) const;

        // softmax interface that is not a pure function, but no manual allocation and no compile-time knowledge needed
        void softmax(const FeatureType* array,
                     TreeEnsembleResponseType* out,
                     TreeEnsembleResponseType baseResponse = defaultBaseResponse) const;

        void write_bin(std::string const& filename) const;

        int nClasses() const { return baseResponses_.size() > 2 ? baseResponses_.size() : 2; }

        std::vector<int> rootIndices_;
        std::vector<CutIndexType> cutIndices_;
        std::vector<FeatureType> cutValues_;
        std::vector<int> leftIndices_;
        std::vector<int> rightIndices_;
        std::vector<TreeResponseType> responses_;
        std::vector<int> treeNumbers_;
        std::vector<TreeEnsembleResponseType> baseResponses_;

    private:
        void evaluate(const FeatureType* array,
                      TreeEnsembleResponseType* out,
                      int nOut,
                      TreeEnsembleResponseType baseResponse) const;

        TreeEnsembleResponseType evaluateBinary(const FeatureType* array, TreeEnsembleResponseType baseResponse) const;
    };

    FastForest load_txt(std::string const& txtpath, std::vector<std::string>& features, int nClasses = 2);
    FastForest load_txt(std::istream& is, std::vector<std::string>& features, int nClasses = 2);
    FastForest load_bin(std::string const& txtpath);
    FastForest load_bin(std::istream& is);
#ifdef EXPERIMENTAL_TMVA_SUPPORT
    FastForest load_tmva_xml(std::string const& xmlpath, std::vector<std::string>& features);
#endif

}  // namespace fastforest

#endif