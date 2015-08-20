/** \file    math_sample.h
    \brief   sampling points from a probability distribution
    \date    2013-2015
    \author  Eugene Vasiliev
*/
#pragma once
#include "math_ndim.h"

namespace math{

/** Sample points from an N-dimensional probability distribution function F.
    F should be non-negative in the given region, and the integral of F over this region should exist;
    still better is if F is bounded from above everywhere in the region.
    The output consists of M sampling points from the given region, such that the density
    of points in the neighborhood of any location X is proportional to the value of F(X).
    The samples are drawn from the probability distribution described by F using the standard
    rejection sampling; the key algorithmic challenge is to make it efficient, i.e., minimize
    the number of discarded trial points. This is achieved by adaptive importance sampling method.
    \param[in]  F  is the probability distribution, the dimensionality N of the problem
                is given by F->numVars();
    \param[in]  xlower  is the lower boundary of sampling volume (vector of length N);
    \param[in]  xupper  is the upper boundary of sampling volume;
    \param[in]  numSamples  is the required number of sampling points (M);
    \param[in]  numBins (optional) is the array of numbers of bins per dimension,
                if set to NULL then a default binning scheme is assumed.
                Useful in the case when the function is known to have little variation in
                some of its variables, in which case the number of bins in the corresponding
                dimension may be set to 1 or another small number, saving some memory.
    \param[out] samples  will be filled by samples, i.e. contain the matrix of M rows and N columns;
    \param[out] numTrialPoints (optional) if not NULL, will store the actual number of function
                evaluations (so that the efficiency of sampling is estimated as the ratio
                numSamples/numTrialPoints);
    \param[out] integral (optional) if not NULL, will store the Monte Carlo estimate of the integral
                of F over the given region (this could be compared with the exact value, if known,
                to estimate the bias/error in sampling scheme);
    \param[out] interror (optional) if not NULL, will store the error estimate of the integral.
 */
void sampleNdim(const IFunctionNdim& F, const double xlower[], const double xupper[],
    const unsigned int numSamples, const unsigned int* numBins,
    Matrix<double>& samples, int* numTrialPoints=0, double* integral=0, double* interror=0);

}  // namespace