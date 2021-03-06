#include "galaxymodel.h"
#include "math_core.h"
#include "math_sample.h"
#include "math_specfunc.h"
#include "math_spline.h"
#include "math_linalg.h"
#include "actions_torus.h"
#include "smart.h"
#include "utils.h"
#include <cmath>
#include <stdexcept>

namespace galaxymodel{

namespace{   // internal definitions

//------- HELPER ROUTINES -------//

/** convert from scaled velocity variables to the actual velocity.
    \param[in]  vars are the scaled variables: |v|/velmag, theta/Pi, phi/(2*Pi),
    where the latter two quantities specify the orientation of velocity vector 
    in spherical coordinates centered at a given point, and
    \param[in]  velmag is the maximum magnutude of velocity (equal to the escape velocity).
    \param[out] jac (optional) if not NULL, output the jacobian of transformation.
    \return  three components of velocity in cylindrical coordinates
*/
inline coord::VelCyl unscaleVelocity(const double vars[], const double velmag, double* jac=0)
{
    const double costheta = cos(M_PI * vars[1]);
    const double sintheta = sin(M_PI * vars[1]);
    const double vel = vars[0]*velmag;
    if(jac)
        *jac = 2*M_PI*M_PI * vel*vel * velmag * sintheta;
    return coord::VelCyl(
        vel * sintheta * cos(2*M_PI * vars[2]),
        vel * sintheta * sin(2*M_PI * vars[2]),
        vel * costheta);
}

/** compute the escape velocity at a given position in the given ponential */
inline double escapeVel(const coord::PosCyl& pos, const potential::BasePotential& poten)
{
    if(pow_2(pos.R)+pow_2(pos.z) == INFINITY)
        return 0;
    const double Phi_inf = 0;   // assume that the potential is zero at infinity
    const double vesc = sqrt(2. * (Phi_inf - poten.value(pos)));
    if(!isFinite(vesc)) {
        throw std::invalid_argument("Error in computing moments: escape velocity is undetermined at "
            "R="+utils::toString(pos.R)+", z="+utils::toString(pos.z)+", phi="+utils::toString(pos.phi)+
            " (Phi="+utils::toString(poten.value(pos))+")");
    }
    return vesc;
}

/** convert from scaled position/velocity coordinates to the real ones.
    The coordinates in cylindrical system are scaled in the same way as for 
    the density integration; the velocity magnitude is scaled with local escape velocity.
    If needed, also provide the jacobian of transformation.
*/
inline coord::PosVelCyl unscalePosVel(const double vars[], 
    const potential::BasePotential& poten, double* jac=0)
{
    // 1. determine the position from the first three scaled variables
    double jacPos=0;
    const coord::PosCyl pos = potential::unscaleCoords(vars, jac==NULL ? NULL : &jacPos);
    // 2. determine the velocity from the second three scaled vars
    const double velmag = escapeVel(pos, poten);
    const coord::VelCyl vel = unscaleVelocity(vars+3, velmag, jac);
    if(jac!=NULL)
        *jac *= jacPos;
    return coord::PosVelCyl(pos, vel);
}

/** convert scaled z-coordinate into the actual z;
    if necessary, also provide the jacobian of transformation */
inline double unscaleZ(double zscaled, double *jac=NULL) {
    if(jac!=NULL)
        *jac = pow_2(1/(1-zscaled)) + pow_2(1/zscaled);
    return 1/(1-zscaled) - 1/zscaled;   // jacobian of coordinate transformation
}

//------- HELPER CLASSES FOR MULTIDIMENSIONAL INTEGRATION OF DF -------//

/** Base helper class for integrating the distribution function over the position/velocity space.
    Various tasks in this module boil down to computing the integrals or sampling the values of DF
    over the (x,v) space, where the DF is expressed in terms of actions.
    This involves the following steps:
    1) scaled variables in N-dimensional unit cube are transformed to the actual (x,v);
    2) x,v are transformed to actions (J);
    3) the value of DF f(J) is computed;
    4) one or more quantities that are products of f(J) times something
    (e.g., velocity components) are returned to the integration or sampling routines.
    These tasks differ in the first and the last steps, and also in the number of dimensions
    that the integration/sampling is carried over. This diversity is handled by the class
    hierarchy descending from DFIntegrandNdim, where the base class performs the steps 2 and 3,
    and the derived classes implement virtual methods `unscaleVars()` and `outputValues()`,
    which are responsible for the steps 1 and 4, respectively.
    The derived classes also specify the dimensions of integration space (numVars)
    and the number of simultaneously computed quantities (numValues).
    The action finder for performing the step 2 and the DF used in the step 3 are provided
    as members of the GalaxyModel structure.
*/
class DFIntegrandNdim: public math::IFunctionNdim {
public:
    explicit DFIntegrandNdim(const GalaxyModel& _model) :
        model(_model) {}

    /** compute one or more moments of distribution function. */
    virtual void eval(const double vars[], double values[]) const
    {
        double dfval;  // value of distribution function
        double jac;    // jacobian of variable transformation
        coord::PosVelCyl posvel;
        try{
            // 1. get the position/velocity components in cylindrical coordinates
            posvel = unscaleVars(vars, &jac);
            if(jac == 0) {  // we can't compute actions, but pretend that DF*jac is zero
                outputValues(posvel, 0, values);
                return;
            }

            // 2. determine the actions
            actions::Actions act = model.actFinder.actions(posvel);

            // 3. compute the value of distribution function times the jacobian
            dfval = isFinite(act.Jr + act.Jz + act.Jphi) ? model.distrFunc.value(act) * jac : 0.;

            if(!isFinite(dfval))
                throw std::runtime_error("DF is not finite");
        }
        catch(std::exception& e) {
            if(utils::verbosityLevel >= utils::VL_VERBOSE) {
                utils::msg(utils::VL_VERBOSE, "DFIntegrandNdim", std::string(e.what()) +
                    " at R="+utils::toString(posvel.R)  +", z="   +utils::toString(posvel.z)+
                    ", phi="+utils::toString(posvel.phi)+", vR="  +utils::toString(posvel.vR)+
                    ", vz=" +utils::toString(posvel.vz) +", vphi="+utils::toString(posvel.vphi));
            }
            dfval = 0;
        }

        // 4. output the value(s) to the integration routine
        outputValues(posvel, dfval, values);
    }

    /** convert from scaled variables used in the integration routine 
        to the actual position/velocity point.
        \param[in]  vars  is the array of scaled variables;
        \param[out] jac (optional)  is the jacobian of transformation, if NULL it is not computed;
        \return  the position and velocity in cylindrical coordinates.
    */
    virtual coord::PosVelCyl unscaleVars(const double vars[], double* jac=0) const = 0;

protected:
    /** output the value(s) computed at a given point to the integration routine.
        \param[in]  point  is the position/velocity point;
        \param[in]  dfval  is the value of distribution function at this point;
        \param[out] values is the array of one or more values that are computed
    */
    virtual void outputValues(const coord::PosVelCyl& point, const double dfval, 
        double values[]) const = 0;

    const GalaxyModel& model;  ///< reference to the galaxy model to work with
};


/** helper class for computing the projected distribution function at a given point in x,y,vz space  */
class DFIntegrandProjected: public DFIntegrandNdim, public math::IFunctionNoDeriv {
public:
    DFIntegrandProjected(const GalaxyModel& _model, double _R, double _vz, double _vz_error) :
        DFIntegrandNdim(_model), R(_R), vz(_vz), vz_error(_vz_error) {}

    /// return v^2-vz^2 (used in setting the integration limits by root-finding)
    virtual double value(double zscaled) const {
        return -vz*vz + (zscaled==0 || zscaled==1 ? 0 : 
            -2*model.potential.value(coord::PosCyl(R, unscaleZ(zscaled), 0)));
    }

    /// input variables define the missing components of position and velocity
    /// to be integrated over, suitably scaled: z, vx, vy
    virtual coord::PosVelCyl unscaleVars(const double vars[], double* jac=0) const {
        double z   = unscaleZ(vars[0], jac);
        double vz1 = vz;
        if(vz_error!=0)  // add velocity error sampled from Gaussian c.d.f.
            vz1 += M_SQRT2 * vz_error * math::erfinv(2*vars[3]-1);
        double v2 = (vars[0]==0 || vars[0]==1 ? 0 : 
            - 2*model.potential.value(coord::PosCyl(R, z, 0)) - vz1*vz1);   // -2 Phi(r) - vz^2
        if(v2<=0) {    // we're outside the allowed range of z
            if(jac!=NULL)
                *jac = 0;
            return coord::PosVelCyl(R, 0, 0, 0, vz, 0);
        }
        double v = sqrt(v2) * vars[1];
        if(jac!=NULL)
            *jac *= 2*M_PI * v2 * vars[1];    // jacobian of velocity transformation
        return coord::PosVelCyl(R, z, 0,
            v * cos(2*M_PI*vars[2]), vz1, v * sin(2*M_PI*vars[2]));
    }

protected:
    double R, vz, vz_error;
    virtual unsigned int numVars()   const { return vz_error==0 ? 3 : 4; }
    virtual unsigned int numValues() const { return 1; }

    /// output array contains one element - the value of DF
    virtual void outputValues(const coord::PosVelCyl& , const double dfval, 
        double values[]) const {
        values[0] = dfval;
    }
};


/** helper class for computing the moments of distribution function
    (surface density and line-of-sight velocity dispersion) at a given point in x,y plane  */
class DFIntegrandProjectedMoments: public DFIntegrandNdim {
public:
    DFIntegrandProjectedMoments(const GalaxyModel& _model, double _R) :
        DFIntegrandNdim(_model), R(_R) {}

    /// input variables define the z-coordinate and all three velocity components, suitably scaled
    virtual coord::PosVelCyl unscaleVars(const double vars[], double* jac=0) const {
        coord::PosCyl pos(R, unscaleZ(vars[0], jac), 0);
        const double velmag = escapeVel(pos, model.potential);
        double jacVel;
        const coord::VelCyl vel = unscaleVelocity(vars+1, velmag, &jacVel);
        if(jac!=NULL)
            *jac = velmag==0 ? 0 : *jac * jacVel;
        return coord::PosVelCyl(pos, vel);
    }

protected:
    double R;
    virtual unsigned int numVars()   const { return 4; }
    virtual unsigned int numValues() const { return 2; }

    /// output array contains two elements - the value of DF
    /// and its second moment with line-of-sight velocity
    virtual void outputValues(const coord::PosVelCyl& pv, const double dfval, 
        double values[]) const {
        values[0] = dfval;
        values[1] = dfval * pow_2(pv.vz);
    }
};


/** helper class for integrating the distribution function over the entire 6d phase space */
class DFIntegrand6dim: public DFIntegrandNdim {
public:
    DFIntegrand6dim(const GalaxyModel& _model) :
        DFIntegrandNdim(_model) {}

    /// input variables define 6 components of position and velocity, suitably scaled
    virtual coord::PosVelCyl unscaleVars(const double vars[], double* jac=0) const { 
        return unscalePosVel(vars, model.potential, jac);
    }

protected:
    virtual unsigned int numVars()   const { return 6; }
    virtual unsigned int numValues() const { return 1; }

    /// output array contains one element - the value of DF
    virtual void outputValues(const coord::PosVelCyl& , const double dfval, 
        double values[]) const {
        values[0] = dfval;
    }
};


/** specification of the velocity moments of DF to be computed at a single point in space
    (a combination of them is given by bitwise OR) */
enum OperationMode {
    OP_VEL1MOM=1,  ///< first moments of velocity
    OP_VEL2MOM=2   ///< second moments of velocity
};

/** helper class for integrating the distribution function over velocity at a fixed position */
class DFIntegrandAtPoint: public math::IFunctionNdim {
public:
    DFIntegrandAtPoint(const GalaxyModel& _model, const coord::PosCyl& _point, OperationMode _mode) :
        model(_model), numCompDF(model.distrFunc.numValues()),
        point(_point), v_esc(escapeVel(point, model.potential)), mode(_mode),
        numOutVal(1 + (mode&OP_VEL1MOM ? 3 : 0) + (mode&OP_VEL2MOM ? 6 : 0)) {}

    virtual void eval(const double vars[], double values[]) const
    {
        // 1. get the position/velocity components in cylindrical coordinates
        double jac;    // jacobian of variable transformation
        coord::PosVelCyl posvel(point, unscaleVelocity(vars, v_esc, &jac));

        if(jac == 0) {  // we can't compute actions, but pretend that DF*jac is zero
            for(unsigned int i=0; i<numCompDF * numOutVal; i++)
                values[i] = 0;
            return;
        }

        // 2. determine the actions
        actions::Actions acts = model.actFinder.actions(posvel);

        // 3. compute the value(s) of distribution function
        model.distrFunc.eval(acts, values);

        // 4. output the value(s) of DF, multiplied by various combinations of velocity components:
        // {f, f*vR, f*vz, f*vphi, f*vR^2, f*vz^2, f*vphi^2, f*vR*vz, f*vR*vphi, f*vz*vphi },
        // depending on the mode of operation.
        for(unsigned int ic=0; ic<numCompDF; ic++) {  // loop over components of DF
            double dfval = values[ic] * jac;
            values[ic]   = dfval;
            unsigned int im=1;      // index of the output moment, increases with each stored value
            if(mode & OP_VEL1MOM) {
                values[ic + numCompDF * (im++)] = dfval * posvel.vR;
                values[ic + numCompDF * (im++)] = dfval * posvel.vz;
                values[ic + numCompDF * (im++)] = dfval * posvel.vphi;
            }
            if(mode & OP_VEL2MOM) {
                values[ic + numCompDF * (im++)] = dfval * posvel.vR * posvel.vR;
                values[ic + numCompDF * (im++)] = dfval * posvel.vz * posvel.vz;
                values[ic + numCompDF * (im++)] = dfval * posvel.vphi * posvel.vphi;
                values[ic + numCompDF * (im++)] = dfval * posvel.vR * posvel.vz;
                values[ic + numCompDF * (im++)] = dfval * posvel.vR * posvel.vphi;
                values[ic + numCompDF * (im++)] = dfval * posvel.vz * posvel.vphi;
            }
        }
    }

    /// dimension of the input array (3 scaled velocity components)
    virtual unsigned int numVars()   const { return 3; }

    /// dimension of the output array
    virtual unsigned int numValues() const { return numCompDF * numOutVal; }

private:
    const GalaxyModel& model;     ///< reference to the galaxy model to work with
    const unsigned int numCompDF; ///< number of DF components (if model is multicomponent), or 1
    const coord::PosCyl point;    ///< fixed position
    const double v_esc;           ///< escape velocity at this position
    const OperationMode mode;     ///< determines which moments of DF to compute
    const unsigned int numOutVal; ///< number of output values for each component of DF
};


/** Helper class for constructing histograms of velocity distribution */
template <int N>
class DFIntegrandVelDist: public DFIntegrandNdim {
public:
    DFIntegrandVelDist(const GalaxyModel& _model,
        const coord::PosCyl& _point, bool _projected, 
        const math::BsplineInterpolator1d<N>& _bsplVR,
        const math::BsplineInterpolator1d<N>& _bsplVz,
        const math::BsplineInterpolator1d<N>& _bsplVphi) :
    DFIntegrandNdim(_model), point(_point),
    projected(_projected), v_esc(escapeVel(point, model.potential)),
    bsplVR(_bsplVR), bsplVz(_bsplVz), bsplVphi(_bsplVphi),
    NR(bsplVR.numValues()), Nz(bsplVz.numValues()), Ntotal(1 + NR + Nz + bsplVphi.numValues())
    {}

    virtual unsigned int numVars()   const { return projected ? 4 : 3; }
    virtual unsigned int numValues() const { return Ntotal; }
private:
    const coord::PosCyl point;       ///< position
    const bool projected;            ///< if true, only use R and phi and integrate over z
    const double v_esc;              ///< escape velocity at this position (if not projected)
    const math::BsplineInterpolator1d<N>& bsplVR, bsplVz, bsplVphi;
    const unsigned int NR, Nz, Ntotal;

    /// input variables define the z-coordinate and all three velocity components, suitably scaled
    virtual coord::PosVelCyl unscaleVars(const double vars[], double* jac=0) const {
        if(projected) {
            coord::PosCyl pos(point.R, unscaleZ(vars[0], jac), point.phi);
            const double velmag = escapeVel(pos, model.potential);
            double jacVel;
            const coord::VelCyl vel = unscaleVelocity(vars+1, velmag, &jacVel);
            if(jac!=NULL)
                *jac = velmag==0 ? 0 : *jac * jacVel;
            return coord::PosVelCyl(pos, vel);
        } else
            return coord::PosVelCyl(point, unscaleVelocity(vars, v_esc, jac));
    }

    /// output the weighted integrals over basis functions;
    /// we scan only half of the (v_R, v_z) plane, and add the same contributions to (-v_R, -v_z),
    /// since the actions and hence the value of f(J) do not change with this inversion
    virtual void outputValues(const coord::PosVelCyl& pv, const double dfval, double values[]) const {
        std::fill(values, values+Ntotal, 0);
        double valRp[N+1], valRm[N+1], valzp[N+1], valzm[N+1], valphi[N+1];
        unsigned int 
            iRp  = bsplVR.  nonzeroComponents( pv.vR,  0, valRp),
            iRm  = bsplVR.  nonzeroComponents(-pv.vR,  0, valRm),
            izp  = bsplVz.  nonzeroComponents( pv.vz,  0, valzp),
            izm  = bsplVz.  nonzeroComponents(-pv.vz,  0, valzm),
            iphi = bsplVphi.nonzeroComponents(pv.vphi, 0, valphi);
        values[0] = dfval;
        for(int i=0; i<=N; i++) {
            values[1 + i + iRp]           += dfval * valRp[i]*.5;
            values[1 + i + iRm]           += dfval * valRm[i]*.5;
            values[1 + i + izp  + NR]     += dfval * valzp[i]*.5;
            values[1 + i + izm  + NR]     += dfval * valzm[i]*.5;
            values[1 + i + iphi + NR + Nz] = dfval * valphi[i];
        }
    }
};

/** Solve the equation for amplitudes of B-spline expansion of the velocity distribution function.
    The VDF is represented as  \f$  f(v) = \sum_i A_i B_i(v)  \f$,  where B_i(v) are the B-spline basis
    functions and A_i are the amplitudes to be found by solving the following linear system:
    \f$  \int f(v) B_j(v) dv = \sum_i A_i  [ \int B_i(v) B_j(v) dv ] = C_j  \f$,
    where C_j is the RHS vector computed through the integration of f(v) weighted with each
    basis function, and the overlap matrix in square brackets is provided by the B-spline object.
    Even though the RHS by definition is non-negative, the solution vector is not guaranteed to be so
    (unless the matrix is diagonal, which is the case only for N=0, i.e., a histogram representation);
    that is, the interpolated f(v) may attain unphysical negative values.
    We employ an additional measure that helps to reduce this effect:
    if the order of B-spline interpolator is larger than zero (i.e., it's not a simple histogram), and
    if the endpoints of the velocity interval are at the escape velocity (meaning that f(v) must be 0),
    we enforce the amplitudes of the first and the last basis functions to be zero.
    In this case, of course, the number of variables in the system is less than the number of equations,
    so it is solved in the least-square sense using the singular-value decomposition, instead of
    the standard Cholesky decomposition for a full-rank symmetric matrix.
    \param[in]  bspl  is the B-spline basis in the 1d velocity space;
    \param[in]  rhs   is the RHS of the linear system;
    \param[in]  vesc  is the escape velocity: if the endpoints of the B-spline interval are at or beyond
    the escape velocity, we force the corresponding amplitudes to be zero.
    \return  the vector of amplitudes.
*/
template<int N>
std::vector<double> solveForAmplitudes(const math::BsplineInterpolator1d<N>& bspl,
    const std::vector<double>& rhs, double vesc)
{
    math::BandMatrix<double> bandMat = math::FiniteElement1d<N>(bspl.xvalues()).computeProjMatrix();
    int size = bandMat.rows();
#if 0
    // another possibility is to use a linear or quadratic optimization solver that enforce
    // non-negativity constraints on the solution vector, at the same time minimizing the deviation
    // from the exact solution. This is perhaps the cleanest approach, but it requires
    // the optimization libraries to be included at the compile time, which we can't guarantee.
    return math::quadraticOptimizationSolveApprox(bandMat, rhs,
        std::vector<double>()          /*linear penalty for variables is absent*/,
        math::BandMatrix<double>()     /*quadratic penalty for variables is absent*/,
        std::vector<double>()          /*linear penalty for constraint violation is absent*/,
        std::vector<double>(size, 1e9) /*quadratic penalty for constraint violation*/,
        std::vector<double>(size, 0.)  /*lower bound on the solution vector*/ );
#endif
    int skipFirst = (N >= 1 && size>2 && math::fcmp(bspl.xmin(), -vesc, 1e-8) <= 0);
    int skipLast  = (N >= 1 && size>2 && math::fcmp(bspl.xmax(),  vesc, 1e-8) >= 0);
    if(skipFirst+skipLast==0)
        return math::solveBand(bandMat, rhs);
    // otherwise create another matrix with fewer columns (copy row-by-row from the original matrix)
    math::Matrix<double> fullMat(bandMat);
    math::Matrix<double> reducedMat(size, size-skipFirst-skipLast, 0.);
    for(int i=0; i<size; i++)
        std::copy(&fullMat(i, skipFirst), &fullMat(i, size-skipLast), &reducedMat(i, 0));
    // use the SVD to solve the rank-deficient system
    std::vector<double> sol = math::SVDecomp(reducedMat).solve(rhs);
    // append the skipped amplitudes
    if(skipFirst)
        sol.insert(sol.begin(), 0);
    if(skipLast)
        sol.insert(sol.end(),   0);
    return sol;
}


}  // unnamed namespace

//------- DRIVER ROUTINES -------//

void computeMoments(const GalaxyModel& model, const coord::PosCyl& point,
    double* density, coord::VelCyl* velocityFirstMoment, coord::Vel2Cyl* velocitySecondMoment,
    double* densityErr, coord::VelCyl* velocityFirstMomentErr, coord::Vel2Cyl* velocitySecondMomentErr,
    const double reqRelError, const int maxNumEval)
{
    OperationMode mode = static_cast<OperationMode>(
        (velocityFirstMoment !=NULL ? OP_VEL1MOM : 0) |
        (velocitySecondMoment!=NULL ? OP_VEL2MOM : 0) );
    DFIntegrandAtPoint fnc(model, point, mode);
    // the integration region in scaled velocities
    double xlower[3] = {0, 0, 0};
    double xupper[3] = {1, 1, 1};
    // the values of integrals and their error estimates
    std::vector<double> result(fnc.numValues()), error(fnc.numValues());

    math::integrateNdim(fnc, xlower, xupper, reqRelError, maxNumEval, &result[0], &error[0]);

    // store the results
    unsigned int numCompDF = model.distrFunc.numValues();
    for(unsigned int ic=0; ic<numCompDF; ic++) {
        if(density!=NULL) {
            density[ic] = result[ic];
            if(densityErr!=NULL)
                densityErr[ic] = error[ic];
        }
        double densVal = result[ic], densRelErr2 = pow_2(error[ic]/result[ic]);
        unsigned int im=1;  // index of the computed moment in the results array
        if(velocityFirstMoment!=NULL) {
            velocityFirstMoment[ic] = densVal==0 ? coord::VelCyl(0,0,0) : coord::VelCyl(
                result[ic + (im+0)*numCompDF] / densVal,
                result[ic + (im+1)*numCompDF] / densVal,
                result[ic + (im+2)*numCompDF] / densVal);
            if(velocityFirstMomentErr!=NULL) {
                // relative errors in moments are summed in quadrature from errors in rho and rho*v
                velocityFirstMomentErr[ic].vR =
                    fabs(velocityFirstMoment[ic].vR) * sqrt(densRelErr2 +
                    pow_2(error[ic + (im+0)*numCompDF] / result[ic + (im+0)*numCompDF]) );
                velocityFirstMomentErr[ic].vz =
                    fabs(velocityFirstMoment[ic].vz) * sqrt(densRelErr2 +
                    pow_2(error[ic + (im+1)*numCompDF] / result[ic + (im+1)*numCompDF]) );
                velocityFirstMomentErr[ic].vphi =
                    fabs(velocityFirstMoment[ic].vphi) * sqrt(densRelErr2 +
                    pow_2(error[ic + (im+2)*numCompDF] / result[ic + (im+2)*numCompDF]) );
            }
            im+=3;
        }
        if(velocitySecondMoment!=NULL) {
            velocitySecondMoment[ic].vR2    = densVal ? result[ic + (im+0)*numCompDF] / densVal : 0;
            velocitySecondMoment[ic].vz2    = densVal ? result[ic + (im+1)*numCompDF] / densVal : 0;
            velocitySecondMoment[ic].vphi2  = densVal ? result[ic + (im+2)*numCompDF] / densVal : 0;
            velocitySecondMoment[ic].vRvz   = densVal ? result[ic + (im+3)*numCompDF] / densVal : 0;
            velocitySecondMoment[ic].vRvphi = densVal ? result[ic + (im+4)*numCompDF] / densVal : 0;
            velocitySecondMoment[ic].vzvphi = densVal ? result[ic + (im+5)*numCompDF] / densVal : 0;
            if(velocitySecondMomentErr!=NULL) {
                velocitySecondMomentErr[ic].vR2 =
                    fabs(velocitySecondMoment[ic].vR2) * sqrt(densRelErr2 +
                    pow_2(error[ic + (im+0)*numCompDF] / result[ic + (im+0)*numCompDF]) );
                velocitySecondMomentErr[ic].vz2 =
                    fabs(velocitySecondMoment[ic].vz2) * sqrt(densRelErr2 +
                    pow_2(error[ic + (im+1)*numCompDF] / result[ic + (im+1)*numCompDF]) );
                velocitySecondMomentErr[ic].vphi2 =
                    fabs(velocitySecondMoment[ic].vphi2) * sqrt(densRelErr2 +
                    pow_2(error[ic + (im+2)*numCompDF] / result[ic + (im+2)*numCompDF]) );
                velocitySecondMomentErr[ic].vRvz =
                    fabs(velocitySecondMoment[ic].vRvz) * sqrt(densRelErr2 +
                    pow_2(error[ic + (im+3)*numCompDF] / result[ic + (im+3)*numCompDF]) );
                velocitySecondMomentErr[ic].vRvphi =
                    fabs(velocitySecondMoment[ic].vRvphi) * sqrt(densRelErr2 +
                    pow_2(error[ic + (im+4)*numCompDF] / result[ic + (im+4)*numCompDF]) );
                velocitySecondMomentErr[ic].vzvphi =
                    fabs(velocitySecondMoment[ic].vzvphi) * sqrt(densRelErr2 +
                    pow_2(error[ic + (im+5)*numCompDF] / result[ic + (im+5)*numCompDF]) );
            }
        }
    }
}


template <int N>
double computeVelocityDistribution(const GalaxyModel& model,
    const coord::PosCyl& point, bool projected,
    const std::vector<double>& gridVR,
    const std::vector<double>& gridVz,
    const std::vector<double>& gridVphi,
    std::vector<double>& amplVR,
    std::vector<double>& amplVz,
    std::vector<double>& amplVphi,
    const double reqRelError, const int maxNumEval)
{
    double vesc = sqrt(-2 * model.potential.value(
        projected ? coord::PosCyl(point.R, 0, point.phi) : point) );   // escape velocity
    math::BsplineInterpolator1d<N> bsplVR(gridVR), bsplVz(gridVz), bsplVphi(gridVphi);
    const unsigned int NR = bsplVR.numValues(), Nz = bsplVz.numValues(), Nphi = bsplVphi.numValues();
    DFIntegrandVelDist<N> fnc(model, point, projected, bsplVR, bsplVz, bsplVphi);

    // the integration region [scaled z, 3 components of scaled velocity]
    double xlower[4] = {0, 0, 0, 0};
    double xupper[4] = {1, 1, 1, 0.5};  // scan only half of {v_R,v_z} plane, since the VDF is symmetric
    // the values of integrals and their error estimates
    std::vector<double> result(fnc.numValues());

    math::integrateNdim(fnc,
        projected? xlower : xlower+1,   // the 0th dimension (z) only used in the case of projected VDF,
        projected? xupper : xupper+1,   // otherwise only three components of scaled velocity
        reqRelError, maxNumEval, &result.front());

    // compute the amplitudes of un-normalized VDF
    amplVR   = solveForAmplitudes<N>(bsplVR,
        std::vector<double>(result.begin()+1, result.begin()+1+NR), vesc);
    amplVz   = solveForAmplitudes<N>(bsplVz,
        std::vector<double>(result.begin()+1+NR, result.begin()+1+NR+Nz), vesc);
    amplVphi = solveForAmplitudes<N>(bsplVphi,
        std::vector<double>(result.begin()+1+NR+Nz, result.begin()+1+NR+Nz+Nphi), vesc);

    // normalize by the value of density
    double density = result[0];
    math::blas_dmul(1/density, amplVR);
    math::blas_dmul(1/density, amplVz);
    math::blas_dmul(1/density, amplVphi);
    return density*2;  // factor of two because we only integrated over half-space
}

// force compilation of several template instantiations
template double computeVelocityDistribution<0>(const GalaxyModel&, const coord::PosCyl&, bool,
    const std::vector<double>&, const std::vector<double>&, const std::vector<double>&,
    std::vector<double>&, std::vector<double>&, std::vector<double>&, const double, const int);
template double computeVelocityDistribution<1>(const GalaxyModel&, const coord::PosCyl&, bool,
    const std::vector<double>&, const std::vector<double>&, const std::vector<double>&,
    std::vector<double>&, std::vector<double>&, std::vector<double>&, const double, const int);
template double computeVelocityDistribution<3>(const GalaxyModel&, const coord::PosCyl&, bool,
    const std::vector<double>&, const std::vector<double>&, const std::vector<double>&,
    std::vector<double>&, std::vector<double>&, std::vector<double>&, const double, const int);

double computeProjectedDF(const GalaxyModel& model,
    const double R, const double vz, const double vz_error,
    const double reqRelError, const int maxNumEval)
{
    double xlower[4] = {0, 0, 0, 0};  // integration region in scaled variables
    double xupper[4] = {1, 1, 1, 1};
    DFIntegrandProjected fnc(model, R, vz, vz_error);
    if(vz_error==0) {  // in this case we may put tighter limits on the integration interval in z
        xlower[0] = math::findRoot(fnc, 0, 0.5, 1e-8);  // set the lower and upper limits for integration
        xupper[0] = math::findRoot(fnc, 0.5, 1, 1e-8);  // to the region where v^2-vz^2>0
    }
    double result;
    math::integrateNdim(fnc, xlower, xupper, reqRelError, maxNumEval, &result);
    return result;
}


void computeProjectedMoments(const GalaxyModel& model, const double R,
    double& surfaceDensity, double& losvdisp, double* surfaceDensityErr, double* losvdispErr,
    const double reqRelError, const int maxNumEval)
{
    double xlower[4] = {0, 0, 0, 0};  // integration region in scaled variables
    double xupper[4] = {1, 1, 1, 1};
    DFIntegrandProjectedMoments fnc(model, R);
    double result[2], error[2];
    math::integrateNdim(fnc, xlower, xupper, reqRelError, maxNumEval, result, error);
    surfaceDensity = result[0];
    losvdisp = result[1] / result[0];
    if(surfaceDensityErr)
        *surfaceDensityErr = error[0];
    if(losvdispErr)
        *losvdispErr = sqrt(pow_2(error[0]/result[0]*result[1]) + pow_2(error[1]));
}


particles::ParticleArrayCyl generateActionSamples(
    const GalaxyModel& model, const size_t nSamp, std::vector<actions::Actions>* actsOutput)
{
    // first sample points from the action space:
    // we use nAct << nSamp  distinct values for actions, and construct tori for these actions;
    // then each torus is sampled with nAng = nSamp/nAct  distinct values of angles,
    // and the action/angles are converted to position/velocity points
    size_t nAng = std::min<size_t>(nSamp/100+1, 16);   // number of sample angles per torus
    size_t nAct = nSamp / nAng + 1;
    std::vector<actions::Actions> actions;

    // do the sampling in actions space
    double totalMass, totalMassErr;
    df::sampleActions(model.distrFunc, nAct, actions, &totalMass, &totalMassErr);
    nAct = actions.size();   // could be different from requested?
    //double totalMass = distrFunc.totalMass();
    double pointMass = totalMass / (nAct*nAng);

    // next sample angles from each torus
    particles::ParticleArrayCyl points;
    if(actsOutput!=NULL)
        actsOutput->clear();
    for(size_t t=0; t<nAct && points.size()<nSamp; t++) {
        actions::ActionMapperTorus torus(model.potential, actions[t]);
        for(size_t a=0; a<nAng; a++) {
            actions::Angles ang;
            ang.thetar   = 2*M_PI*math::random();
            ang.thetaz   = 2*M_PI*math::random();
            ang.thetaphi = 2*M_PI*math::random();
            points.add(torus.map(actions::ActionAngles(actions[t], ang)), pointMass);
            if(actsOutput!=NULL)
                actsOutput->push_back(actions[t]);
        }
    }
    return points;
}


particles::ParticleArrayCyl generatePosVelSamples(
    const GalaxyModel& model, const size_t numSamples)
{
    DFIntegrand6dim fnc(model);
    math::Matrix<double> result;      // sampled scaled coordinates/velocities
    double totalMass, errorMass;      // total normalization of the DF and its estimated error
    double xlower[6] = {0,0,0,0,0,0}; // boundaries of sampling region in scaled coordinates
    double xupper[6] = {1,1,1,1,1,1};
    math::sampleNdim(fnc, xlower, xupper, numSamples, result, NULL, &totalMass, &errorMass);
    const double pointMass = totalMass / result.rows();
    particles::ParticleArrayCyl points;
    points.data.reserve(result.rows());
    for(size_t i=0; i<result.rows(); i++) {
        double scaledvars[6] = {result(i,0), result(i,1), result(i,2),
            result(i,3), result(i,4), result(i,5)};
        // transform from scaled vars (array of 6 numbers) to real pos/vel
        points.add(fnc.unscaleVars(scaledvars), pointMass);
    }
    return points;
}


particles::ParticleArray<coord::PosCyl> generateDensitySamples(
    const potential::BaseDensity& dens, const size_t numPoints)
{
    potential::DensityIntegrandNdim fnc(dens, /*require the values of density to be non-negative*/true);  
    math::Matrix<double> result;      // sampled scaled coordinates
    double totalMass, errorMass;      // total mass and its estimated error
    double xlower[3] = {0,0,0};       // boundaries of sampling region in scaled coordinates
    double xupper[3] = {1,1,1};
    math::sampleNdim(fnc, xlower, xupper, numPoints, result, NULL, &totalMass, &errorMass);
    const double pointMass = totalMass / result.rows();
    particles::ParticleArray<coord::PosCyl> points;
    points.data.reserve(result.rows());
    for(size_t i=0; i<result.rows(); i++) {
        // if the system is axisymmetric, phi is not provided by the sampling routine
        double scaledvars[3] = {result(i,0), result(i,1), 
            fnc.axisym ? math::random() : result(i,2)};
        // transform from scaled coordinates to the real ones, and store the point into the array
        points.add(fnc.unscaleVars(scaledvars), pointMass);
    }
    return points;
}

}  // namespace