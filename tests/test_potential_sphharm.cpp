#include "potential_analytic.h"
#include "potential_dehnen.h"
#include "potential_cylspline.h"
#include "potential_sphharm.h"
#include "potential_factory.h"
#include "particles_io.h"
#include "debug_utils.h"
#include "utils.h"
#include "utils_config.h"
#include <fstream>
#include <cstdlib>
#include <cstdio>

/// define test suite in terms of points for various coord systems
const int numtestpoints=8;
const double pos_sph[numtestpoints][3] = {   // order: R, theta, phi
    {1  , 2  , 3},   // ordinary point
    {2  , 1  , 0},   // point in x-z plane
    {1  , 0  , 0},   // point along z axis
    {2  , 3.14159,-1},   // point along z axis, z<0
    {0.5, 1.5707963, 1.5707963},  // point along y axis
    {111, 0.7, 2},   // point at a large radius
    {.01, 2.5,-1},   // point at a small radius origin
    {0  , 0  , 0} }; // point at origin

const bool output = false;

/// write potential coefs into file, load them back and create a new potential from these coefs
const potential::BasePotential* write_read(const potential::BasePotential& pot)
{
    std::string coefFile("test_potential_sphharm");
    coefFile += getCoefFileExtension(pot);
    writePotentialCoefs(coefFile, pot);
    const potential::BasePotential* newpot = potential::readPotentialCoefs(coefFile);
    std::remove(coefFile.c_str());
    return newpot;
}

/// create a triaxial Hernquist model
void make_hernquist(int nbody, double q, double p, particles::PointMassArrayCar& out)
{
    out.data.clear();
    for(int i=0; i<nbody; i++) {
        double m = rand()*1.0/RAND_MAX;
        double r = 1/(1/sqrt(m)-1);
        double costheta = rand()*2.0/RAND_MAX - 1;
        double sintheta = sqrt(1-pow_2(costheta));
        double phi = rand()*2*M_PI/RAND_MAX;
        out.add(coord::PosVelCar(r*sintheta*cos(phi), r*sintheta*sin(phi)*q, r*costheta*p, 0, 0, 0), 1./nbody);
    }
}

const potential::BasePotential* create_from_file(
    const particles::PointMassArrayCar& points, const std::string& potType)
{
    const std::string fileName = "test.txt";
    particles::BaseIOSnapshot* snap = particles::createIOSnapshotWrite(
        "Text", fileName, units::ExternalUnits());
    snap->writeSnapshot(points);
    delete snap;
    const potential::BasePotential* newpot = NULL;

    // illustrates two possible ways of creating a potential from points
    if(potType == "BasisSetExp") {
        particles::PointMassArrayCar pts;
        particles::readSnapshot(fileName, units::ExternalUnits(), pts);
        newpot = new potential::BasisSetExp(
            1.0, /*alpha*/
            20,  /*numCoefsRadial*/
            4,   /*numCoefsAngular*/
            pts, /*points*/
            potential::ST_TRIAXIAL);  /*symmetry (default value)*/
    } else {
        // a rather lengthy way of setting parameters, used only for illustration:
        // normally these would be read from an INI file or from command line;
        // to create an instance of potential expansion of a known type, 
        // use directly its constructor as shown above
        utils::KeyValueMap params;
        params.set("file", fileName);
        params.set("type", potType);
        params.set("numCoefsRadial", 20);
        params.set("numCoefsAngular", 4);
        params.set("numCoefsVertical", 20);
        params.set("Density", "Nbody");
        newpot = potential::createPotential(params);
    }
    std::remove(fileName.c_str());
    std::remove((fileName+potential::getCoefFileExtension(potType)).c_str());
    return newpot;
}

particles::PointMassArrayCar points;  // sampling points

/// compare potential and its derivatives between the original model and its spherical-harmonic approximation
bool test_suite(const potential::BasePotential* pp, const potential::BasePotential& orig, double eps_pot)
{
    const potential::BasePotential& p = *pp;
    bool ok=true;
    const potential::BasePotential* newpot = write_read(p);
    double gamma = getInnerDensitySlope(orig);
    std::cout << "\033[1;32m---- testing "<<p.name()<<" with "<<
        (isSpherical(orig) ? "spherical " : "triaxial ") <<orig.name()<<
        " (gamma="<<gamma<<") ----\033[0m\n";
    const char* err = "\033[1;31m **\033[0m";
    std::string fileName = std::string("test_") + p.name() + "_" + orig.name() + 
        "_gamma" + utils::convertToString(gamma);
    if(output)
        writePotentialCoefs(fileName + getCoefFileExtension(p), p);
    for(int ic=0; ic<numtestpoints; ic++) {
        double pot, pot_orig;
        coord::GradCyl der,  der_orig;
        coord::HessCyl der2, der2_orig;
        coord::PosSph point(pos_sph[ic][0], pos_sph[ic][1], pos_sph[ic][2]);
        newpot->eval(toPosCyl(point), &pot, &der, &der2);
        orig.eval(toPosCyl(point), &pot_orig, &der_orig, &der2_orig);
        double eps_der = eps_pot*100/point.r;
        double eps_der2= eps_der*10;
        bool pot_ok = (pot==pot) && fabs(pot-pot_orig)<eps_pot;
        bool der_ok = point.r==0 || equalGrad(der, der_orig, eps_der);
        bool der2_ok= point.r==0 || equalHess(der2, der2_orig, eps_der2);
        ok &= pot_ok && der_ok && der2_ok;
        std::cout << "Point:  " << point << 
            "Phi: " << pot << " (orig:" << pot_orig << (pot_ok?"":err) << ")\n"
            "Force sphharm: " << der  << "\nForce origin.: " << der_orig  << (der_ok ?"":err) << "\n"
            "Deriv sphharm: " << der2 << "\nDeriv origin.: " << der2_orig << (der2_ok?"":err) << "\n";
    }
    if(output) {
        std::ofstream strmSample((fileName+".samples").c_str());
        for(unsigned int ic=0; ic<points.size(); ic++) {
            double pot, pot_orig;
            coord::GradCyl der,  der_orig;
            coord::HessCyl der2, der2_orig;
            newpot->eval(toPosCyl(points.point(ic)), &pot, &der, &der2);
            orig.eval(toPosCyl(points.point(ic)), &pot_orig, &der_orig, &der2_orig);
            strmSample << toPosSph(points.point(ic)).r << "\t" <<
            fabs((pot-pot_orig)/pot_orig) << "\t" <<
            fabs((der.dR-der_orig.dR)/der_orig.dR) << "\t" <<
            fabs((der.dz-der_orig.dz)/der_orig.dz) << "\t" <<
            fabs(1-newpot->density(points.point(ic))/orig.density(points.point(ic))) << "\n";
        }
    }
    delete newpot;
    return ok;
}

int main() {
    srand(42);
    bool ok=true;
    make_hernquist(100000, 0.8, 0.6, points);
    const potential::BasePotential* p=0;

    // spherical, cored
    const potential::Plummer plum(10., 5.);
    p = new potential::BasisSetExp(0., 30, 2, plum);
    ok &= test_suite(p, plum, 1e-5);
    delete p;
    p = new potential::SplineExp(20, 2, plum);
    ok &= test_suite(p, plum, 1e-5);
    delete p;
    // this forces potential to be computed via integration of density over volume
    p = new potential::CylSplineExp(20, 20, 0, static_cast<const potential::BaseDensity&>(plum));
    ok &= test_suite(p, plum, 1e-4);
    delete p;

    // mildly triaxial, cuspy
    const potential::Dehnen deh15(3., 1.2, 0.8, 0.6, 1.5);
    p = new potential::BasisSetExp(2., 20, 6, deh15);
    ok &= test_suite(p, deh15, 2e-4);
    delete p;
    p = new potential::SplineExp(20, 6, deh15);
    ok &= test_suite(p, deh15, 2e-4);
    delete p;

    // mildly triaxial, cored
    const potential::Dehnen deh0(1., 1., 0.8, 0.6, 0.);
    p = new potential::BasisSetExp(1., 20, 6, deh0);
    ok &= test_suite(p, deh0, 5e-5);
    delete p;
    p = new potential::SplineExp(20, 6, deh0);
    ok &= test_suite(p, deh0, 5e-5);
    delete p;
    p = new potential::CylSplineExp(20, 20, 6, static_cast<const potential::BaseDensity&>(deh0));
    ok &= test_suite(p, deh0, 1e-4);
    delete p;

    // mildly triaxial, created from N-body samples
    const potential::Dehnen hernq(1., 1., 0.8, 0.6, 1.0);
    p = create_from_file(points, potential::BasisSetExp::myName());
    ok &= test_suite(p, hernq, 2e-2);
    delete p;
    p = new potential::SplineExp(20, 4, points, potential::ST_TRIAXIAL);  // or maybe create_from_file(points, "SplineExp");
    ok &= test_suite(p, hernq, 2e-2);
    delete p;
    p = create_from_file(points, potential::CylSplineExp::myName());
    ok &= test_suite(p, hernq, 2e-2);
    delete p;
    if(ok)
        std::cout << "ALL TESTS PASSED\n";
    return 0;
}