/*=========================================================================
 *
 *  Copyright NumFOCUS
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0.txt
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *=========================================================================*/
#ifndef itkMaxPhaseCorrelationOptimizer_hxx
#define itkMaxPhaseCorrelationOptimizer_hxx

#include "itkMaxPhaseCorrelationOptimizer.h"

#include "itkImageRegionConstIterator.h"
#include "itkImageRegionIteratorWithIndex.h"

#include <cmath>
#include <type_traits>

/*
 * \author Jakub Bican, jakub.bican@matfyz.cz, Department of Image Processing,
 *         Institute of Information Theory and Automation,
 *         Academy of Sciences of the Czech Republic.
 *
 */

namespace itk
{
template <typename TRegistrationMethod>
void
MaxPhaseCorrelationOptimizer<TRegistrationMethod>::PrintSelf(std::ostream & os, Indent indent) const
{
  Superclass::PrintSelf(os, indent);
  os << indent << "MaxCalculator: " << m_MaxCalculator << std::endl;
  // os << indent << "PeakInterpolationMethod: " << m_PeakInterpolationMethod << std::endl;
  os << indent << "MergePeaks: " << m_MergePeaks << std::endl;
  os << indent << "ZeroSuppression: " << m_ZeroSuppression << std::endl;
  os << indent << "PixelDistanceTolerance: " << m_PixelDistanceTolerance << std::endl;
}

template <typename TRegistrationMethod>
void
MaxPhaseCorrelationOptimizer<TRegistrationMethod>::SetPeakInterpolationMethod(
  const PeakInterpolationMethodEnum peakInterpolationMethod)
{
  if (this->m_PeakInterpolationMethod != peakInterpolationMethod)
  {
    this->m_PeakInterpolationMethod = peakInterpolationMethod;
    this->Modified();
  }
}

template <typename TRegistrationMethod>
void
MaxPhaseCorrelationOptimizer<TRegistrationMethod>::ComputeOffset()
{
  ImageConstPointer input = static_cast<ImageType *>(this->GetInput(0));
  ImageConstPointer fixed = static_cast<ImageType *>(this->GetInput(1));
  ImageConstPointer moving = static_cast<ImageType *>(this->GetInput(2));

  OffsetType offset;
  offset.Fill(0);

  if (!input)
  {
    return;
  }

  const typename ImageType::RegionType wholeImage = input->GetLargestPossibleRegion();
  const typename ImageType::SizeType   size = wholeImage.GetSize();
  const typename ImageType::IndexType  oIndex = wholeImage.GetIndex();

  const typename ImageType::SpacingType spacing = fixed->GetSpacing();
  const typename ImageType::PointType   fixedOrigin = fixed->GetOrigin();
  const typename ImageType::PointType   movingOrigin = moving->GetOrigin();

  // create the image which will be biased towards the expected solution
  // other pixels get their value reduced by multiplication with
  // e^(-f*(d/s)^2), where f is distancePenaltyFactor,
  // d is pixel's distance, and s is approximate image size
  typename ImageType::Pointer iAdjusted = ImageType::New();
  iAdjusted->CopyInformation(input);
  iAdjusted->SetRegions(input->GetBufferedRegion());
  iAdjusted->Allocate(false);

  typename ImageType::IndexType adjustedSize;
  typename ImageType::IndexType directExpectedIndex;
  typename ImageType::IndexType mirrorExpectedIndex;
  double                        imageSize2 = 0.0; // image size, squared
  for (unsigned d = 0; d < ImageDimension; d++)
  {
    adjustedSize[d] = size[d] + oIndex[d];
    imageSize2 += adjustedSize[d] * adjustedSize[d];
    directExpectedIndex[d] = (movingOrigin[d] - fixedOrigin[d]) / spacing[d] + oIndex[d];
    mirrorExpectedIndex[d] = (movingOrigin[d] - fixedOrigin[d]) / spacing[d] + adjustedSize[d];
  }

  double distancePenaltyFactor = 0.0;
  if (m_PixelDistanceTolerance == 0) // up to about half image size
  {
    distancePenaltyFactor = -10.0 / imageSize2;
  }
  else // up to about five times the provided tolerance
  {
    distancePenaltyFactor = std::log(0.9) / (m_PixelDistanceTolerance * m_PixelDistanceTolerance);
  }

  MultiThreaderBase * mt = this->GetMultiThreader();
  mt->ParallelizeImageRegion<ImageDimension>(
    wholeImage,
    [&](const typename ImageType::RegionType & region) {
      ImageRegionConstIterator<ImageType>     iIt(input, region);
      ImageRegionIteratorWithIndex<ImageType> oIt(iAdjusted, region);
      IndexValueType                          zeroDist2 =
        100 * m_PixelDistanceTolerance * m_PixelDistanceTolerance; // round down to zero further from this
      for (; !oIt.IsAtEnd(); ++iIt, ++oIt)
      {
        typename ImageType::IndexType ind = oIt.GetIndex();
        IndexValueType                dist = 0;
        for (unsigned d = 0; d < ImageDimension; d++)
        {
          IndexValueType distDirect = (directExpectedIndex[d] - ind[d]) * (directExpectedIndex[d] - ind[d]);
          IndexValueType distMirror = (mirrorExpectedIndex[d] - ind[d]) * (mirrorExpectedIndex[d] - ind[d]);
          if (distDirect <= distMirror)
          {
            dist += distDirect;
          }
          else
          {
            dist += distMirror;
          }
        }

        typename ImageType::PixelType pixel;
        if (m_PixelDistanceTolerance > 0 && dist > zeroDist2)
        {
          pixel = 0;
        }
        else // evaluate the expensive exponential function
        {
          pixel = iIt.Get() * std::exp(distancePenaltyFactor * dist);
#ifndef NDEBUG
          pixel *= 1000; // make the intensities in this image more humane (close to 1.0)
                         // it is really hard to count zeroes after decimal point when comparing pixel intensities
                         // since this images is used to find maxima, absolute values are irrelevant
#endif
        }
        oIt.Set(pixel);
      }
    },
    nullptr);

  WriteDebug(iAdjusted.GetPointer(), "iAdjusted.nrrd");

  if (m_ZeroSuppression > 0.0) // suppress trivial zero solution
  {
    constexpr IndexValueType znSize = 4; // zero neighborhood size, in city-block distance
    mt->ParallelizeImageRegion<ImageDimension>(
      wholeImage,
      [&](const typename ImageType::RegionType & region) {
        ImageRegionIteratorWithIndex<ImageType> oIt(iAdjusted, region);
        for (; !oIt.IsAtEnd(); ++oIt)
        {
          bool                          pixelValid = false;
          typename ImageType::PixelType pixel;
          typename ImageType::IndexType ind = oIt.GetIndex();
          IndexValueType                dist = 0;
          for (unsigned d = 0; d < ImageDimension; d++)
          {
            IndexValueType distD = ind[d] - oIndex[d];
            if (distD > IndexValueType(size[d] / 2)) // wrap around
            {
              distD = size[d] - distD;
            }
            dist += distD;
          }

          if (dist < znSize) // neighborhood of [0,0,...,0] - in case zero peak is blurred
          {
            pixelValid = true;
          }
          else
          {
            for (unsigned d = 0; d < ImageDimension; d++) // lines/sheets of zero indices
            {
              if (ind[d] == oIndex[d]) // one of the indices is "zero"
              {
                pixelValid = true;
              }
            }
          }

          if (pixelValid) // either neighborhood or lines/sheets says update the pixel
          {
            pixel = oIt.Get();
            // avoid the initial steep rise of function x/(1+x) by shifting it by 10
            pixel *= (dist + 10) / (m_ZeroSuppression + dist + 10);
            oIt.Set(pixel);
          }
        }
      },
      nullptr);

    WriteDebug(iAdjusted.GetPointer(), "iAdjustedZS.nrrd");
  }

  m_MaxCalculator->SetImage(iAdjusted);
  if (m_MergePeaks)
  {
    m_MaxCalculator->SetN(std::ceil(this->m_Offsets.size() / 2) *
                          (static_cast<unsigned>(std::pow(3, ImageDimension)) - 1));
  }
  else
  {
    m_MaxCalculator->SetN(this->m_Offsets.size());
  }

  try
  {
    m_MaxCalculator->ComputeMaxima();
  }
  catch (ExceptionObject & err)
  {
    itkDebugMacro("exception caught during execution of max calculator - passing ");
    throw err;
  }

  this->m_Confidences = m_MaxCalculator->GetMaxima();
  typename MaxCalculatorType::IndexVector indices = m_MaxCalculator->GetIndicesOfMaxima();
  itkAssertOrThrowMacro(this->m_Confidences.size() == indices.size(),
                        "Maxima and their indices must have the same number of elements");
  std::greater<PixelType> compGreater;
  auto zeroBound = std::upper_bound(this->m_Confidences.begin(), this->m_Confidences.end(), 0.0, compGreater);
  if (zeroBound != this->m_Confidences.end()) // there are some non-positive values in here
  {
    unsigned i = zeroBound - this->m_Confidences.begin();
    this->m_Confidences.resize(i);
    indices.resize(i);
  }

  if (m_MergePeaks > 0) // eliminate indices belonging to the same blurry peak
  {
    unsigned i = 1;
    while (i < indices.size())
    {
      unsigned k = 0;
      while (k < i)
      {
        // calculate maximum distance along any dimension
        SizeValueType dist = 0;
        for (unsigned d = 0; d < ImageDimension; d++)
        {
          SizeValueType d1 = std::abs(indices[i][d] - indices[k][d]);
          if (d1 > size[d] / 2) // wrap around
          {
            d1 = size[d] - d1;
          }
          dist = std::max(dist, d1);
        }
        if (dist <= m_MergePeaks)
        {
          break;
        }
        ++k;
      }

      if (k < i) // k is nearby
      {
        this->m_Confidences[k] += this->m_Confidences[i]; // join amplitudes
        this->m_Confidences.erase(this->m_Confidences.begin() + i);
        indices.erase(indices.begin() + i);
      }
      else // examine next index
      {
        ++i;
      }
    }

    // now we need to re-sort the values
    std::vector<unsigned> sIndices;
    sIndices.reserve(this->m_Confidences.size());
    for (i = 0; i < this->m_Confidences.size(); i++)
    {
      sIndices.push_back(i);
    }
    std::sort(sIndices.begin(), sIndices.end(), [this](unsigned a, unsigned b) {
      return this->m_Confidences[a] > this->m_Confidences[b];
    });

    // now apply sorted order
    typename MaxCalculatorType::ValueVector tMaxs(this->m_Confidences.size());
    typename MaxCalculatorType::IndexVector tIndices(this->m_Confidences.size());
    for (i = 0; i < this->m_Confidences.size(); i++)
    {
      tMaxs[i] = this->m_Confidences[sIndices[i]];
      tIndices[i] = indices[sIndices[i]];
    }
    this->m_Confidences.swap(tMaxs);
    indices.swap(tIndices);
  }

  if (this->m_Offsets.size() > this->m_Confidences.size())
  {
    this->SetOffsetCount(this->m_Confidences.size());
  }
  else
  {
    this->m_Confidences.resize(this->m_Offsets.size());
    indices.resize(this->m_Offsets.size());
  }

  // double confidenceFactor = 1.0 / this->m_Confidences[0];

  for (unsigned m = 0; m < this->m_Confidences.size(); m++)
  {
    using ContinuousIndexType = ContinuousIndex<OffsetScalarType, ImageDimension>;
    ContinuousIndexType maxIndex = indices[m];

    if (m_PeakInterpolationMethod != PeakInterpolationMethodEnum::None) // interpolate the peak
    {
      typename ImageType::PixelType y0, y1 = this->m_Confidences[m], y2;
      typename ImageType::IndexType tempIndex = indices[m];

      for (unsigned i = 0; i < ImageDimension; i++)
      {
        tempIndex[i] = maxIndex[i] - 1;
        if (!wholeImage.IsInside(tempIndex))
        {
          tempIndex[i] = maxIndex[i];
          continue;
        }
        y0 = iAdjusted->GetPixel(tempIndex);
        tempIndex[i] = maxIndex[i] + 1;
        if (!wholeImage.IsInside(tempIndex))
        {
          tempIndex[i] = maxIndex[i];
          continue;
        }
        y2 = iAdjusted->GetPixel(tempIndex);
        tempIndex[i] = maxIndex[i];

        OffsetScalarType omega, theta, ratio;
        switch (m_PeakInterpolationMethod)
        {
          case PeakInterpolationMethodEnum::Parabolic:
            maxIndex[i] += (y0 - y2) / (2 * (y0 - 2 * y1 + y2));
            break;
          case PeakInterpolationMethodEnum::Cosine:
            ratio = (y0 + y2) / (2 * y1);
            if (m > 0) // clip to -0.999... to 0.999... range
            {
              ratio = std::min(ratio, 1.0 - std::numeric_limits<OffsetScalarType>::epsilon());
              ratio = std::max(ratio, -1.0 + std::numeric_limits<OffsetScalarType>::epsilon());
            }
            omega = std::acos(ratio);
            theta = std::atan((y0 - y2) / (2 * y1 * std::sin(omega)));
            maxIndex[i] -= ::itk::Math::one_over_pi * theta / omega;
            break;
          default:
            itkAssertInDebugAndIgnoreInReleaseMacro("Unknown interpolation method");
            break;
        } // switch PeakInterpolationMethod
      }   // for ImageDimension
    }     // if Interpolation != None

    for (unsigned i = 0; i < ImageDimension; ++i)
    {
      OffsetScalarType directOffset = (movingOrigin[i] - fixedOrigin[i]) - 1 * spacing[i] * (maxIndex[i] - oIndex[i]);
      OffsetScalarType mirrorOffset =
        (movingOrigin[i] - fixedOrigin[i]) - 1 * spacing[i] * (maxIndex[i] - adjustedSize[i]);
      if (std::abs(directOffset) <= std::abs(mirrorOffset))
      {
        offset[i] = directOffset;
      }
      else
      {
        offset[i] = mirrorOffset;
      }
    }

    // this->m_Confidences[m] *= confidenceFactor; // normalize - highest confidence will be 1.0
#ifdef NDEBUG
    this->m_Confidences[m] *= 1000.0; // make the intensities more humane (close to 1.0)
#endif

    this->m_Offsets[m] = offset;
  }
}

} // end namespace itk

#endif
