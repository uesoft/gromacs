/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 2011-2016, by the GROMACS development team, led by
 * Mark Abraham, David van der Spoel, Berk Hess, and Erik Lindahl,
 * and including many others, as listed in the AUTHORS file in the
 * top-level source directory and at http://www.gromacs.org.
 *
 * GROMACS is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 *
 * GROMACS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GROMACS; if not, see
 * http://www.gnu.org/licenses, or write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA.
 *
 * If you want to redistribute modifications to GROMACS, please
 * consider that scientific software is very special. Version
 * control is crucial - bugs must be traceable. We will be happy to
 * consider code for inclusion in the official distribution, but
 * derived work must not be called official GROMACS. Details are found
 * in the README & COPYING files - if they are missing, get the
 * official version at http://www.gromacs.org.
 *
 * To help us fund GROMACS development, we humbly ask that you cite
 * the research papers on the package. Check out http://www.gromacs.org.
 */
/*! \internal \brief
 * Implements part of the alexandria program.
 * \author  Mohammad Mehdi Ghahremanpour <mohammad.ghahremanpour@icm.uu.se>
 * \author  David van der Spoel <david.vanderspoel@icm.uu.se>
 */

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <random>

#include "gromacs/commandline/pargs.h"
#include "gromacs/commandline/viewit.h"
#include "gromacs/fileio/xvgr.h"
#include "gromacs/hardware/detecthardware.h"
#include "gromacs/listed-forces/bonded.h"
#include "gromacs/mdlib/force.h"
#include "gromacs/mdlib/mdatoms.h"
#include "gromacs/mdlib/shellfc.h"
#include "gromacs/mdtypes/commrec.h"
#include "gromacs/statistics/statistics.h"
#include "gromacs/topology/mtop_util.h"
#include "gromacs/utility/arraysize.h"
#include "gromacs/utility/coolstuff.h"
#include "gromacs/utility/cstringutil.h"
#include "gromacs/utility/fatalerror.h"
#include "gromacs/utility/futil.h"
#include "gromacs/utility/smalloc.h"

#include "gentop_core.h"
#include "getmdlogger.h"
#include "gmx_simple_comm.h"
#include "moldip.h"
#include "optparam.h"
#include "poldata.h"
#include "poldata_tables.h"
#include "poldata_xml.h"
#include "tuning_utility.h"

namespace alexandria
{

class OptZeta : public MolDip
{
    using param_type = std::vector<double>;
    
    private:    
    public:
    
        OptZeta(bool  bfitDipole, 
                bool  bfitQuadrupole,
                bool  bFitAlpha,
                real  watoms, 
                char *lot) 
           :
               bDipole_(bfitDipole),
               bQuadrupole_(bfitQuadrupole),
               bFitAlpha_(bFitAlpha),
               watoms_(watoms),
               lot_(lot)
          {};
        
       ~OptZeta() {};
       
        param_type    param_, lower_, upper_, best_;
        param_type    orig_, psigma_, pmean_;
        bool          bDipole_, bQuadrupole_; 
        bool          bFitAlpha_;
        real          watoms_;
        char         *lot_;
    
        double l2_regularizer (double x, 
                               double min, 
                               double max)
       {
           return (x < min) ? (0.5 * gmx::square(x-min)) : ((x > max) ? (0.5 * gmx::square(x-max)) : 0);
       }                              
        
        void polData2TuneZeta();
        
        void tuneZeta2PolData();
        
        void InitOpt(real factor);
                                 
        void calcDeviation();
    
        double objFunction(const double v[]);
        
        void optRun(FILE                   *fp, 
                    FILE                   *fplog, 
                    int                     maxiter,
                    int                     nrun, 
                    real                    stepsize, 
                    int                     seed,
                    const gmx_output_env_t *oenv,
                    int                     nprint, 
                    const char             *xvgconv, 
                    const char             *xvgepot, 
                    real                    temperature, 
                    bool                    bBound);
};

void OptZeta::polData2TuneZeta()
{
    param_.clear();    
    auto *ic = indexCount();
    for (auto ai = ic->beginIndex(); ai < ic->endIndex(); ++ai)
    {   
        if (!ai->isConst())
        {
            auto ei   = pd_.findEem(iChargeDistributionModel_, ai->name());
            GMX_RELEASE_ASSERT(ei != pd_.EndEemprops(), "Cannot find eemprops");       
            auto zeta = ei->getZeta(0);
            if (0 != zeta)
            {
                param_.push_back(std::move(zeta));
            }
            else
            {
                gmx_fatal(FARGS, "Zeta is zero for atom %s in %d model\n",
                          ai->name().c_str(), iChargeDistributionModel_);
            } 
            
            if(bFitAlpha_)
            {
                auto alpha = 0.0;
                auto sigma = 0.0;
                if (pd_.getAtypePol(ai->name(), &alpha, &sigma))
                {
                    if (0 != alpha)
                    {
                        param_.push_back(std::move(alpha));
                    }
                    else
                    {
                        gmx_fatal(FARGS, "Polarizability is zero for atom %s\n",
                                  ai->name().c_str());
                    }
                }
            }           
        }      
    }
    if (bOptHfac_)
    {
        param_.push_back(std::move(hfac_));
    }       
}

void OptZeta::tuneZeta2PolData()
{
    int   n = 0;
    char  zstr[STRLEN];
    char  z_sig[STRLEN];
    char  buf[STRLEN];
    char  buf_sig[STRLEN];
        
    auto *ic = indexCount();
    for (auto ai = ic->beginIndex(); ai < ic->endIndex(); ++ai)
    {
        if (!ai->isConst())
        {
            auto ei = pd_.findEem(iChargeDistributionModel_, ai->name());
            GMX_RELEASE_ASSERT(ei != pd_.EndEemprops(), "Cannot find eemprops");
            std::string qstr   = ei->getQstr();
            std::string rowstr = ei->getRowstr();
            zstr[0]  = '\0';
            z_sig[0] = '\0';
            auto nzeta  = ei->getNzeta();
            auto zeta   = param_[n];
            auto sigma  = psigma_[n++];
            for (auto i = 0; i < nzeta; i++)
            {
                sprintf(buf, "%g ", zeta);
                sprintf(buf_sig, "%g ", sigma);
                strcat(zstr, buf);
                strcat(z_sig, buf_sig);
            }                
            ei->setRowZetaQ(rowstr, zstr, qstr);
            ei->setZetastr(zstr);
            ei->setZeta_sigma(z_sig);
            
            
            if (bFitAlpha_)
            {
                std::string ptype;
                if (pd_.atypeToPtype(ai->name(), ptype))
                {
                    pd_.setPtypePolarizability(ptype, param_[n], psigma_[n]);
                    n++;
                }
                else
                {
                    gmx_fatal(FARGS, "No Ptype for atom type %s\n",
                              ai->name().c_str());
                }
            }
        }               
    }
    if (bOptHfac_)
    {
        hfac_ = param_[n++];
    }   
}

void OptZeta::InitOpt(real  factor)
{
    polData2TuneZeta();

    orig_.resize(param_.size(), 0);
    best_.resize(param_.size(), 0);
    lower_.resize(param_.size(), 0);
    upper_.resize(param_.size(), 0);
    psigma_.resize(param_.size(), 0);
    pmean_.resize(param_.size(), 0);
    if (factor < 1)
    {
        factor = 1/factor;
    }
    for (size_t i = 0; (i < param_.size()); i++)
    {
        best_[i]  = orig_[i] = param_[i];
        lower_[i] = orig_[i]/factor;
        upper_[i] = orig_[i]*factor;
    }
}

void OptZeta::calcDeviation()
{
    int    j;
    double qtot = 0;
    real   rrms = 0;
    real   wtot = 0; 

    if (PAR(cr_))
    {
        gmx_bcast(sizeof(bFinal_), &bFinal_, cr_);
    }
    if (PAR(cr_) && !bFinal_)
    {
        pd_.broadcast(cr_);
    }   
    for (j = 0; j < ermsNR; j++)
    {
        ener_[j] = 0;
    }    
    for (auto &mymol : mymol_)
    {        
        if ((mymol.eSupp_ == eSupportLocal) ||
            (bFinal_ && (mymol.eSupp_ == eSupportRemote)))
        {
            mymol.Qgresp_.updateZeta(&mymol.topology_->atoms, pd_);
            mymol.Qgresp_.optimizeCharges();  
            if (nullptr != mymol.shellfc_)
            {   
                if (bFitAlpha_)
                {
                    mymol.UpdateIdef(pd_, eitPOLARIZATION);  
                }
                mymol.computeForces(nullptr, cr_);
                mymol.Qgresp_.updateAtomCoords(mymol.state_->x);
            }
            qtot = 0;
            for (j = 0; j < mymol.topology_->atoms.nr; j++)
            {
                auto atomnr = mymol.topology_->atoms.atom[j].atomnumber;
                auto qq     = mymol.Qgresp_.getAtomCharge(j);
                qtot       += qq;
                mymol.mtop_->moltype[0].atoms.atom[j].q  = 
                mymol.mtop_->moltype[0].atoms.atom[j].qB = 
                mymol.topology_->atoms.atom[j].q         =
                mymol.topology_->atoms.atom[j].qB        = qq;
                if (mymol.topology_->atoms.atom[j].ptype == eptAtom ||
                    mymol.topology_->atoms.atom[j].ptype == eptNucleus)
                {
                    auto q_H        = 0 ? (nullptr != mymol.shellfc_) : 1;
                    auto q_OFSClBrI = 0 ? (nullptr != mymol.shellfc_) : 2;                    
                    if (((qq < q_H) && (atomnr == 1)) ||
                        ((qq > q_OFSClBrI) && ((atomnr == 8)  || (atomnr == 9) ||
                                               (atomnr == 16) || (atomnr == 17) ||
                                               (atomnr == 35) || (atomnr == 53))))
                    {
                            ener_[ermsCHARGE] += gmx::square(qq);
                    }
                }
                    
            } 
            ener_[ermsCHARGE] += gmx::square(qtot - mymol.molProp()->getCharge());                     
            mymol.Qgresp_.calcPot();
            ener_[ermsESP]    += convert2gmx(mymol.Qgresp_.getRms(&wtot, &rrms), eg2cHartree_e);                                   
            if (bDipole_)
            {
                mymol.CalcDipole();
                if (bQM_)
                {
                    for (auto mm = 0; mm < DIM; mm++)
                    {                    
                        ener_[ermsMU] += gmx::square(mymol.mu_calc_[mm] - mymol.mu_elec_[mm]);
                    }
                }
                else
                {
                    ener_[ermsMU] += gmx::square(mymol.dip_calc_ - mymol.dip_exp_);
                }
            }                          
            if (bQuadrupole_)
            {
                mymol.CalcQuadrupole();
                for (auto mm = 0; mm < DIM; mm++)
                {
                    if (bfullTensor_)
                    {
                        for (auto nn = 0; nn < DIM; nn++)
                        {
                            ener_[ermsQUAD] += gmx::square(mymol.Q_calc_[mm][nn] - mymol.Q_elec_[mm][nn]);
                        }
                    }
                    else
                    {
                        ener_[ermsQUAD] += gmx::square(mymol.Q_calc_[mm][mm] - mymol.Q_elec_[mm][mm]);
                    }
                }
            }
        }
    }    
    if (PAR(cr_) && !bFinal_)
    {
        gmx_sum(ermsNR, ener_, cr_);
    }    
    if (MASTER(cr_))
    {
        for (j = 0; j < ermsTOT; j++)
        {
            ener_[ermsTOT] += ((fc_[j]*ener_[j])/nmol_support_);
        }
    }   
    if (nullptr != debug && MASTER(cr_))
    {
        fprintf(debug, "ENER:");
        for (j = 0; j < ermsNR; j++)
        {
            fprintf(debug, "  %8.3f", ener_[j]);
        }
        fprintf(debug, "\n");
    }    
}

double OptZeta::objFunction(const double v[])
{    
    double bounds  = 0;
    int    n       = 0;
    
    auto np = param_.size();
    for (size_t i = 0; i < np; i++)
    {
        param_[i] = v[i];
    }
    tuneZeta2PolData();    
    auto *ic = indexCount();
    for (auto ai = ic->beginIndex(); ai < ic->endIndex(); ++ai)
    {
        if (!ai->isConst())
        {                       
            auto zeta = param_[n++];
            bounds   += l2_regularizer(zeta, zeta_min_, zeta_max_);
     
        }
    }   
    if (bOptHfac_)
    {
        hfac_ = param_[n++];
        if (hfac_ > hfac0_)
        {
            bounds += 100*gmx::square(hfac_ - hfac0_);
        }
        else if (hfac_ < -(hfac0_))
        {
            bounds += 100*gmx::square(hfac_ + hfac0_);
        }
    }      
    calcDeviation();
    ener_[ermsBOUNDS] += bounds;
    ener_[ermsTOT]    += bounds;    
    return ener_[ermsTOT];
}

void OptZeta::optRun(FILE                   *fp, 
                     FILE                   *fplog, 
                     int                     maxiter,
                     int                     nrun, 
                     real                    stepsize, 
                     int                     seed,
                     const gmx_output_env_t *oenv,
                     int                     nprint, 
                     const char             *xvgconv, 
                     const char             *xvgepot, 
                     real                    temperature, 
                     bool                    bBound)
{
    std::vector<double> optb, opts, optm;
    double              chi2, chi2_min;
    gmx_bool            bMinimum = false;
    
    auto func = [&] (const double v[]) {
        return objFunction(v);
    };    
    if (MASTER(cr_))
    {    
        if (PAR(cr_))
        {
            for (int dest = 1; dest < cr_->nnodes; dest++)
            {
                gmx_send_int(cr_, dest, (nrun*maxiter*param_.size()));
            }
        }        
        chi2 = chi2_min = GMX_REAL_MAX;
        Bayes <double> TuneZeta(func, param_, lower_, upper_, &chi2);
        TuneZeta.Init(xvgconv, xvgepot, oenv, seed, stepsize, 
                      maxiter, nprint,temperature, bBound);                     
        for (auto n = 0; n < nrun; n++)
        {
            if ((nullptr != fp) && (0 == n))
            {
                fprintf(fp, "\nStarting run %d out of %d\n", n, nrun);
            }            
            TuneZeta.simulate();
            TuneZeta.getBestParam(optb);
            TuneZeta.getPsigma(opts);
            TuneZeta.getPmean(optm);
            if (chi2 < chi2_min)
            {
                bMinimum = true;
                for (size_t k = 0; k < param_.size(); k++)
                {
                    best_[k]   = optb[k];
                    pmean_[k]  = optm[k];
                    psigma_[k] = opts[k];
                }
                chi2_min = chi2;
            }
            TuneZeta.setParam(best_);
        }
        if (bMinimum)
        {
            param_    = best_;
            auto emin = objFunction(best_.data());
            if (fplog)
            {
                fprintf(fplog, "\nMinimum rmsd value during optimization: %.3f.\n", sqrt(emin));
                fprintf(fplog, "Statistics of parameters after optimization\n");
                for (size_t k = 0; k < param_.size(); k++)
                {
                    fprintf(fplog, "Parameter %3zu  Best value:%10g  Mean value:%10g  Sigma:%10g\n", 
                            k, best_[k], pmean_[k], psigma_[k]);
                }
            }
        }
    }
    else
    {
        /* S L A V E   N O D E S */
        auto niter = gmx_recv_int(cr_, 0);
        for (auto n = 0; n < niter + 2; n++)
        {
            calcDeviation();
        }
    }    
    bFinal_ = true;
    if(MASTER(cr_))
    {
        chi2 = objFunction(best_.data());;
        if (nullptr != fp)
        {
            fprintf(fp, "rmsd: %4.3f  ermsBOUNDS: %4.3f  after %d run(s)\n",
                    sqrt(chi2), ener_[ermsBOUNDS], nrun);
        }
        if (nullptr != fplog)
        {
            fprintf(fplog, "rmsd: %4.3f   ermsBOUNDS: %4.3f  after %d run(s)\n",
                    sqrt(chi2), ener_[ermsBOUNDS], nrun);
            fflush(fplog);
        }
    }
}
}

int alex_tune_zeta(int argc, char *argv[])
{
    static const char          *desc[] = {
        "tune_zeta reads a series of molecules and corresponding experimental",
        "dipole moments from a file, and tunes parameters in an algorithm",
        "until the experimental dipole moments are reproduced by the",
        "charge generating algorithm AX as implemented in the gentop program.[PAR]",
        "Minima and maxima for the parameters can be set, these are however",
        "not strictly enforced, but rather they are penalized with a harmonic",
        "function, for which the force constant can be set explicitly.[PAR]",
        "At every reinit step parameters are changed by a random amount within",
        "the fraction set by step size, and within the boundaries given",
        "by the minima and maxima. If the [TT]-random[tt] flag is",
        "given a completely random set of parameters is generated at the start",
        "of each run. At reinit steps however, the parameters are only changed",
        "slightly, in order to speed-up local search but not global search."
        "In other words, complete random starts are done only at the beginning of each",
        "run, and only when explicitly requested.[PAR]",
        "The absolut dipole moment of a molecule remains unchanged if all the",
        "atoms swap the sign of the charge. To prevent this kind of mirror",
        "effects a penalty is added to the square deviation ",
        "if hydrogen atoms have a negative charge. Similarly a penalty is",
        "added if atoms from row VI or VII in the periodic table have a positive",
        "charge. The penalty is equal to the force constant given on the command line",
        "time the square of the charge.[PAR]",
        "One of the electronegativities (chi) is redundant in the optimization,",
        "only the relative values are meaningful.",
        "Therefore by default we fix the value for hydrogen to what is written",
        "in the eemprops.dat file (or whatever is given with the [tt]-d[TT] flag).",
        "A suitable value would be 2.3, the original, value due to Pauling,",
        "this can by overridden by setting the [tt]-fixchi[TT] flag to something else (e.g. a non-existing atom).[PAR]",
        "A selection of molecules into a training set and a test set (or ignore set)",
        "can be made using option [TT]-sel[tt]. The format of this file is:[BR]",
        "iupac|Train[BR]",
        "iupac|Test[BR]",
        "iupac|Ignore[BR]",
        "and you should ideally have a line for each molecule in the molecule database",
        "([TT]-f[tt] option). Missing molecules will be ignored."
    };

    t_filenm                    fnm[] = {
        { efDAT, "-f",         "allmols",       ffREAD  },
        { efDAT, "-d",         "gentop",        ffOPTRD },
        { efDAT, "-o",         "tunezeta",      ffWRITE },
        { efDAT, "-sel",       "molselect",     ffREAD  },
        { efXVG, "-table",     "table",         ffOPTRD },
        { efLOG, "-g",         "charges",       ffWRITE },
        { efXVG, "-qhisto",    "q_histo",       ffWRITE },
        { efXVG, "-dipcorr",   "dip_corr",      ffWRITE },
        { efXVG, "-mucorr",    "mu_corr",       ffWRITE },
        { efXVG, "-thetacorr", "theta_corr",    ffWRITE },
        { efXVG, "-espcorr",   "esp_corr",      ffWRITE },
        { efXVG, "-alphacorr", "alpha_corr",    ffWRITE },
        { efXVG, "-isopol",    "isopol_corr",   ffWRITE },
        { efXVG, "-anisopol",  "anisopol_corr", ffWRITE },
        { efXVG, "-conv",      "param-conv",    ffWRITE },
        { efXVG, "-epot",      "param-epot",    ffWRITE },
        { efTEX, "-latex",     "zeta",          ffWRITE }
    };
    
    const  int                  NFILE         = asize(fnm);

    static int                  nrun          = 1;
    static int                  nprint        = 10;
    static int                  maxiter       = 100;
    static int                  reinit        = 0;
    static int                  mindata       = 3;
    static int                  seed          = -1;
    static int                  qcycle        = 1000;
    static real                 qtol          = 1e-6;
    static real                 watoms        = 0;
    static real                 J0_min        = 5;
    static real                 Chi0_min      = 1;
    static real                 zeta_min      = 1;
    static real                 step          = 0.01;
    static real                 hfac          = 0;
    static real                 rDecrZeta     = -1;
    static real                 J0_max        = 30;
    static real                 Chi0_max      = 30;
    static real                 zeta_max      = 10;
    static real                 fc_mu         = 1;
    static real                 fc_bound      = 1;
    static real                 fc_quad       = 1;
    static real                 fc_charge     = 1;
    static real                 fc_esp        = 1;
    static real                 fc_epot       = 0;
    static real                 fc_force      = 0;
    static real                 fc_polar      = 1;
    static real                 th_toler      = 170;
    static real                 ph_toler      = 5;
    static real                 dip_toler     = 0.5;
    static real                 quad_toler    = 5;
    static real                 alpha_toler   = 3;
    static real                 factor        = 0.8;
    static real                 temperature   = 300;
    static real                 efield        = 1;
    static char                *opt_elem      = nullptr;
    static char                *const_elem    = nullptr;
    static char                *fixchi        = (char *)"";
    static char                *lot           = (char *)"B3LYP/aug-cc-pVTZ";
    static gmx_bool             bRandom       = false;
    static gmx_bool             bOptHfac      = false;
    static gmx_bool             bcompress     = false;
    static gmx_bool             bQM           = false;
    static gmx_bool             bPolar        = false;
    static gmx_bool             bZPE          = false;
    static gmx_bool             bfullTensor   = false;
    static gmx_bool             bBound        = false;
    static gmx_bool             bQuadrupole   = false;
    static gmx_bool             bDipole       = false;
    static gmx_bool             bFitAlpha     = false;
    static gmx_bool             bGenVSites    = false;
    static gmx_bool             bQsym          = false;
    static gmx_bool             bZero         = true;  
    static gmx_bool             bGaussianBug  = true;    
    static gmx_bool             bPrintTable   = false; 
    static const char          *cqdist[]      = {nullptr, "AXp", "AXg", "AXs", "AXpp", "AXpg", "AXps", nullptr};
    static const char          *cqgen[]       = {nullptr, "None", "EEM", "ESP", "RESP", nullptr};
    
    t_pargs                     pa[]         = {
        { "-maxiter", FALSE, etINT, {&maxiter},
          "Max number of iterations for optimization" },
        { "-mindata", FALSE, etINT, {&mindata},
          "Minimum number of data points to optimize a polarizability value" },
        { "-nprint",  FALSE, etINT, {&nprint},
          "How often to print the parameters during the simulation" },
        { "-reinit", FALSE, etINT, {&reinit},
          "After this many iterations the search vectors are randomized again. A vlue of 0 means this is never done at all." },
        { "-nrun",   FALSE, etINT,  {&nrun},
          "This many runs will be done, before each run a complete randomization will be done" },
        { "-qm",     FALSE, etBOOL, {&bQM},
          "Use only quantum chemistry results (from the levels of theory below) in order to fit the parameters. If not set, experimental values will be used as reference with optional quantum chemistry results, in case no experimental results are available" },
        { "-lot",    FALSE, etSTR,  {&lot},
          "Use this method and level of theory when selecting coordinates and charges. Multiple levels can be specified which will be used in the order given, e.g.  B3LYP/aug-cc-pVTZ:HF/6-311G**" },          
        { "-fullTensor", FALSE, etBOOL, {&bfullTensor},
          "consider both diagonal and off-diagonal elements of the Q_Calc matrix for optimization" },        
        { "-qdist",   FALSE, etENUM, {cqdist},
          "Model used for charge distribution" },
        { "-qgen",   FALSE, etENUM, {cqgen},
          "Algorithm used for charge generation" },
        { "-qtol",   FALSE, etREAL, {&qtol},
          "Tolerance for assigning charge generation algorithm." },
        { "-qcycle", FALSE, etINT, {&qcycle},
          "Max number of tries for optimizing the charges." },
        { "-fixchi", FALSE, etSTR,  {&fixchi},
          "Electronegativity for this atom type is fixed. Set to FALSE if you want this variable as well, but read the help text above." },
        { "-seed",   FALSE, etINT,  {&seed},
          "Random number seed. If zero, a seed will be generated." },
        { "-j0",    FALSE, etREAL, {&J0_min},
          "Minimum value that J0 (eV) can obtain in fitting" },
        { "-chi0",    FALSE, etREAL, {&Chi0_min},
          "Minimum value that Chi0 (eV) can obtain in fitting" },
        { "-z0",    FALSE, etREAL, {&zeta_min},
          "Minimum value that inverse radius (1/nm) can obtain in fitting" },
        { "-j1",    FALSE, etREAL, {&J0_max},
          "Maximum value that J0 (eV) can obtain in fitting" },
        { "-chi1",    FALSE, etREAL, {&Chi0_max},
          "Maximum value that Chi0 (eV) can obtain in fitting" },
        { "-z1",    FALSE, etREAL, {&zeta_max},
          "Maximum value that inverse radius (1/nm) can obtain in fitting" },
        { "-decrzeta", FALSE, etREAL, {&rDecrZeta},
          "Generate decreasing zeta with increasing row numbers for atoms that have multiple distributed charges. In this manner the 1S electrons are closer to the nucleus than 2S electrons and so on. If this number is < 0, nothing is done, otherwise a penalty is imposed in fitting if the Z2-Z1 < this number." },
        { "-fc_bound",    FALSE, etREAL, {&fc_bound},
          "Force constant in the penalty function for going outside the borders given with the above six options." },
        { "-fc_mu",    FALSE, etREAL, {&fc_mu},
          "Force constant in the penalty function for the magnitude of the dipole components." },
        { "-fc_quad",  FALSE, etREAL, {&fc_quad},
          "Force constant in the penalty function for the magnitude of the quadrupole components." },
        { "-fc_esp",   FALSE, etREAL, {&fc_esp},
          "Force constant in the penalty function for the magnitude of the electrostatic potential." },
        { "-fc_charge",  FALSE, etREAL, {&fc_charge},
          "Force constant in the penalty function for the magnitude of the charges with respect to the ESP charges." },
        { "-fc_epot",  FALSE, etREAL, {&fc_epot},
          "Force constant in the penalty function for the magnitude of the potential energy." },
        { "-fc_force",  FALSE, etREAL, {&fc_force},
          "Force constant in the penalty function for the magnitude of the force." },
        { "-fc_polar",  FALSE, etREAL, {&fc_polar},
          "Force constant in the penalty function for polarizability." },
        { "-step",  FALSE, etREAL, {&step},
          "Step size in parameter optimization. Is used as a fraction of the starting value, should be less than 10%. At each reinit step the step size is updated." },
        { "-opt_elem",  FALSE, etSTR, {&opt_elem},
          "Space-separated list of atom types to optimize, e.g. \"H C Br\". The other available atom types in gentop.dat are left unmodified. If this variable is not set, all elements will be optimized." },
        { "-const_elem",  FALSE, etSTR, {&const_elem},
          "Space-separated list of atom types to include but keep constant, e.g. \"O N\". These atom types from gentop.dat are left unmodified" },
        { "-random", FALSE, etBOOL, {&bRandom},
          "Generate completely random starting parameters within the limits set by the options. This will be done at the very first step and before each subsequent run." },
        { "-watoms", FALSE, etREAL, {&watoms},
          "Weight for the atoms when fitting the charges to the electrostatic potential. The potential on atoms is usually two orders of magnitude larger than on other points (and negative). For point charges or single smeared charges use zero. For point+smeared charges 1 is recommended (the default)." },
        { "-dipole", FALSE, etBOOL, {&bDipole},
          "Calibrate paramters to reproduce dipole moment." },
        { "-quadrupole", FALSE, etBOOL, {&bQuadrupole},
          "Calibrate paramters to reproduce quadrupole tensor." },
        { "-fitalpha", FALSE, etBOOL, {&bFitAlpha},
          "Calibrate atomic polarizability." },
        { "-zero", FALSE, etBOOL, {&bZero},
          "Use molecules with zero dipole in the fit as well" },
        { "-zpe",     FALSE, etBOOL, {&bZPE},
          "Consider zero-point energy from thermochemistry calculations in order to calculate the reference enthalpy of the molecule" },
        { "-hfac",  FALSE, etREAL, {&hfac},
          "Fudge factor to scale the J00 of hydrogen by (1 + hfac * qH). Default hfac is 0, means no fudging." },
        { "-opthfac",  FALSE, etBOOL, {&bOptHfac},
          "[HIDDEN]Optimize the fudge factor to scale the J00 of hydrogen (see above). If set, then [TT]-hfac[tt] set the absolute value of the largest hfac. Above this, a penalty is incurred." },
        { "-dip_toler", FALSE, etREAL, {&dip_toler},
          "Tolerance (Debye) for marking dipole as an outlier in the log file" },
        { "-quad_toler", FALSE, etREAL, {&quad_toler},
          "Tolerance (Buckingham) for marking quadrupole as an outlier in the log file" },
        { "-alpha_toler", FALSE, etREAL, {&alpha_toler},
          "Tolerance (A^3) for marking polarizability as an outlier in the log file" },
        { "-th_toler", FALSE, etREAL, {&th_toler},
          "Minimum angle to be considered a linear A-B-C bond" },
        { "-ph_toler", FALSE, etREAL, {&ph_toler},
          "Maximum angle to be considered a planar A-B-C/B-C-D torsion" },
        { "-compress", FALSE, etBOOL, {&bcompress},
          "Compress output XML file" },
        { "-bgaussquad", FALSE, etBOOL, {&bGaussianBug},
          "[HIDDEN]Work around a bug in the off-diagonal quadrupole components in Gaussian" },
        { "-btex", FALSE, etBOOL, {&bPrintTable},
          "[HIDDEN]Print the latex table for the Gaussian and Slater exponents" },
        { "-factor", FALSE, etREAL, {&factor},
          "Factor for generating random parameters. Parameters will be taken within the limit factor*x - x/factor" },
        { "-bound", FALSE, etBOOL, {&bBound},
          "Impose box-constrains for the optimization. Box constraints give lower and upper bounds for each parameter seperately." },
        { "-temp",    FALSE, etREAL, {&temperature},
          "'Temperature' for the Monte Carlo simulation" },
        { "-genvsites", FALSE, etBOOL, {&bGenVSites},
          "Generate virtual sites. Check and double check." },
        { "-efield",  FALSE, etREAL, {&efield},
          "The magnitude of the external electeric field to calculate polarizability tensor." },
        { "-qsymm",  FALSE, etBOOL, {&bQsym},
          "Symmetrize the charges on symmetric groups, e.g. CH3, NH2." }
    };

    FILE                 *fp;
    gmx_output_env_t     *oenv;
    time_t                my_t;
    MolSelect             gms;
    
    t_commrec     *cr     = init_commrec(); 
    gmx::MDLogger  mdlog  = getMdLogger(cr, stdout);
    gmx_hw_info_t *hwinfo = gmx_detect_hardware(mdlog, cr, false);
    
    if (!parse_common_args(&argc, argv, PCA_CAN_VIEW, NFILE, fnm, asize(pa), pa,
                           asize(desc), desc, 0, nullptr, &oenv))
    {
        sfree(cr);
        return 0;
    }
    if (MASTER(cr))
    {
        printf("There are %d threads/processes.\n", cr->nnodes);
    }    
    if (MASTER(cr))
    {
        fp = gmx_ffopen(opt2fn("-g", NFILE, fnm), "w");

        time(&my_t);
        fprintf(fp, "# This file was created %s", ctime(&my_t));
        fprintf(fp, "# alexandria is part of GROMACS:\n#\n");
        fprintf(fp, "# %s\n#\n", gmx::bromacs().c_str());
    }
    else
    {
        fp = nullptr;
    }    
    if (MASTER(cr))
    {
        gms.read(opt2fn_null("-sel", NFILE, fnm));
    }
    
    ChargeDistributionModel        iChargeDistributionModel   = name2eemtype(cqdist[0]);
    ChargeGenerationAlgorithm      iChargeGenerationAlgorithm = (ChargeGenerationAlgorithm) get_option(cqgen);
    const char                    *tabfn                      = opt2fn_null("-table", NFILE, fnm);
    
    if (iChargeDistributionModel == eqdAXpp  || 
        iChargeDistributionModel == eqdAXpg  || 
        iChargeDistributionModel == eqdAXps)
    {
        bPolar = true;
    }
    
    alexandria::OptZeta opt(bDipole, bQuadrupole, bFitAlpha, watoms, lot);
    opt.Init(cr,
             bQM,
             bGaussianBug,
             iChargeDistributionModel,
             iChargeGenerationAlgorithm,
             rDecrZeta,
             J0_min,
             Chi0_min,
             zeta_min, 
             J0_max,
             Chi0_max,
             zeta_max,
             fc_bound, 
             fc_mu,
             fc_quad,
             fc_charge,
             fc_esp,
             fc_epot,
             fc_force,
             fc_polar,
             fixchi,
             bOptHfac,
             hfac, 
             bPolar,
             false,
             hwinfo,
             bfullTensor,
             mindata,
             bGenVSites);
            
    opt.Read(fp ? fp : (debug ? debug : nullptr),
             opt2fn("-f", NFILE, fnm),
             opt2fn_null("-d", NFILE, fnm),
             bZero,
             opt_elem,
             const_elem,
             lot,
             gms,
             watoms,
             true,
             false,
             false,
             bPolar,
             bZPE,
             tabfn,
             qcycle,
             qtol,
             bQsym);
            
    if (nullptr != fp)
    {
        fprintf(fp, "In the total data set of %zu molecules we have:\n", opt.mymol_.size());
    }
    if (maxiter > 0)
    {
        if (MASTER(cr))
        {
            opt.InitOpt(factor);
        }    
        
        opt.optRun(MASTER(cr) ? stderr : nullptr,
                   fp,
                   maxiter,
                   nrun,
                   step,
                   seed,
                   oenv,
                   nprint,
                   opt2fn("-conv", NFILE, fnm),
                   opt2fn("-epot", NFILE, fnm),
                   temperature,
                   bBound);
    }
    if (MASTER(cr))
    {
        auto *ic = opt.indexCount();
        print_electric_props(fp,  
                             opt.mymol_,
                             opt2fn("-qhisto",    NFILE, fnm),
                             opt2fn("-dipcorr",   NFILE, fnm),
                             opt2fn("-mucorr",    NFILE, fnm),
                             opt2fn("-thetacorr", NFILE, fnm), 
                             opt2fn("-espcorr",   NFILE, fnm),
                             opt2fn("-alphacorr", NFILE, fnm),
                             opt2fn("-isopol",    NFILE, fnm),
                             opt2fn("-anisopol",  NFILE, fnm),
                             dip_toler, 
                             quad_toler,
                             alpha_toler, 
                             oenv,
                             bPolar,
                             bDipole,
                             bQuadrupole,
                             opt.bfullTensor_,
                             ic,
                             opt.hfac_,
                             opt.cr_,
                             efield);
                            
        writePoldata(opt2fn("-o", NFILE, fnm), opt.pd_, bcompress);
        gmx_ffclose(fp);        
        if (bPrintTable)
        {
            FILE        *tp;
            tp = gmx_ffopen(opt2fn("-latex", NFILE, fnm), "w");
            alexandria_poldata_eemprops_table(tp, true, false, opt.pd_);
            gmx_ffclose(tp);
        }       
        done_filenms(NFILE, fnm);
    }
    return 0;
}
