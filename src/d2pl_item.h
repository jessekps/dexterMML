#ifndef DXM_D2PL_ITEM_
#define DXM_D2PL_ITEM_

#include <RcppArmadillo.h>
#include "shared.h"

// contains negative likelihood, gradient and hessian for single dichotomous 2pl item given r0 and r1
// for use in minimizing functions
struct ll_2pl_dich
{
	arma::vec r0,r1,theta;
	int n;

	ll_2pl_dich(double* r1p, double* r0p, double* thetap, const int ni)
	{
		n=ni;
		r1 = arma::vec(r1p,n,false,true);		
		r0 = arma::vec(r0p,n,false,true);
		theta = arma::vec(thetap,n,false,true);	
	}
	
	//returns minus log likelihood
	double operator()(const arma::vec& ab)
	{
		double ll=0;
		const double a = ab[0], b = ab[1];
		for(int i=0;i<n;i++)
		{
			double p = 1/(1+std::exp(-a*(theta[i]-b)));
			ll -= r1[i] * std::log(p) + r0[i] * std::log(1-p);
		}
		if(std::isinf(ll))
		{
			printf("infinity, a: %f, b: %f", a, b);
			fflush(stdout);
			Rcpp::stop("inf ll");
		}
		return ll;	
	}
	
	//returns gradient of minus ll
	void df(const arma::vec& ab, arma::vec& g)
	{	
		g.zeros();
		const double a = ab[0], b = ab[1];
		for(int i=0;i<n;i++)
		{
			double e = std::exp(a*(b-theta[i]));
			g[0] -= (b-theta[i]) * (r0[i] - r1[i]*e)/(e+1);
			g[1] -= a * (r0[i]-r1[i]*e)/(e+1);
		}
		
		if(!g.is_finite())
		{
			printf("infinity in gradient, a: %f, b: %f", a, b);
			fflush(stdout);
			Rcpp::stop("inf gradient");
		}
	}
	// hessian of LL
	void hess(const arma::vec& ab, arma::mat& h, const bool negative=true)
	{
		h.zeros();
		const double a = ab[0], b = ab[1];
		for(int i=0;i<n;i++)
		{
			double e = std::exp(a*(b-theta[i])), t=theta[i];
			h.at(0,0) -= (r0[i]*(e+1) - r0[i] - r1[i]*(e+1)*e - (2*r0[i]-r1[i]*e)*e) \
					* SQR(b-t)/SQR(e+1);
			
			h.at(0,1) -= (a*r0[i]*(t-b) - a*(b-t)*(2*r0[i]-r1[i]*e)*e \
							+ r0[i]*(a*(b-t)+1)*(e+1) \
							- r1[i]*(a*(b-t) + 1)*(e+1)*e) \
							/ SQR(e+1);			
			
			e = std::exp(a*(b+t));
			
			h.at(1,1) += SQR(a)*(r0[i]+r1[i])*e/(std::exp(2*a*b) + std::exp(2*a*t) + 2*e);
		}
		h.at(1,0) = h.at(0,1);
		if(!negative)
			h *= -1;
	}
};

#endif