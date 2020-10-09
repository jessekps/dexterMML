
#include <RcppArmadillo.h>
#include "minimize.h"

#pragma omp declare reduction( + : arma::mat : omp_out += omp_in ) \
initializer( omp_priv = omp_orig )

#pragma omp declare reduction( + : arma::vec : omp_out += omp_in ) \
initializer( omp_priv = omp_orig )

using namespace arma;
using Rcpp::Named;



// no groups


inline double SQR(double v){ return v*v; }


vec gaussian_pts(const double mu, const double s, const vec& theta)
{
	const int nt = theta.n_elem;
	vec out(nt);
	double half = (theta[1] - theta[0])/2;

	for(int i=0; i<nt; i++)
		out[i] = R::pnorm(theta[i]+half,mu,s,true,false) - R::pnorm(theta[i]-half,mu,s,true,false);

	out = out / accu(out);
	
	return out;
}



void estep_2pl_dich(const vec& a, const vec& b, const ivec& pni, const ivec& pcni, const ivec& pi, const ivec& px, 
				const vec& theta, mat& r0, mat& r1, vec& thetabar, double& sumsig2, const double mu=0, const double sigma=1)
{
	const int nit = a.n_elem, nt = theta.n_elem, np = pni.n_elem;
	mat itrace(nt,nit);
	
	const vec posterior0 = gaussian_pts(mu,sigma,theta);
	
	r0.zeros();
	r1.zeros();
	
	for(int i=0; i<nit; i++)
		itrace.col(i) = 1/(1+exp(-a[i]*(theta-b[i])));
		
	vec sigma2(nt, fill::zeros);
	double dev = 0;
	
#pragma omp parallel
	{
		vec posterior(nt);
# pragma omp for reduction(+:r0,r1,sigma2, dev)
		for(int p=0; p<np;p++)
		{
			for(int i=0; i<nt;i++)
				posterior[i] = posterior0[i];
				
			for(int indx = pcni[p]; indx<pcni[p+1]; indx++)
			{
				if(px[indx] == 1)
					posterior %= itrace.col(pi[indx]);
				else
					posterior %= 1-itrace.col(pi[indx]);
			}	
			double sp = accu(posterior);
			posterior = posterior / sp;
			thetabar[p] = accu(posterior % theta);
			dev += std::log(sp*.6);
			sigma2 += posterior; 
			
			for(int indx = pcni[p]; indx<pcni[p+1]; indx++)
			{
				if(px[indx] == 1)
					r1.col(pi[indx]) += posterior;
				else
					r0.col(pi[indx]) += posterior;
			}		
		}
	}
	dev*=-2;
	printf("deviance: %f\n",dev);
	fflush(stdout);
	sumsig2 = accu(sigma2 % square(theta));
}


struct ll_2pl_dich
{
	vec r0,r1,theta;
	int n;

	ll_2pl_dich(double* r1p, double* r0p, double* thetap, const int ni)
	{
		n=ni;
		r1 = vec(r1p,n,false,true);		
		r0 = vec(r0p,n,false,true);
		theta = vec(thetap,n,false,true);	
	}
	
	//returns minus log likelihood
	double operator()(const vec& ab)
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
	void df(const vec& ab, vec& g)
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
	
	void hess(const vec& ab, mat& h)
	{
		h.zeros();
		const double a = ab[0], b = ab[1];
		for(int i=0;i<n;i++)
		{
			double e = std::exp(a*(b-theta[i])), t=theta[i];
			h.at(0,0) += (r0[i]*(e+1) - r0[i] - r1[i]*(e+1)*e - (2*r0[i]-r1[i]*e)*e) \
					* SQR(b-t)/SQR(e+1);
			
			h.at(0,1) += (a*r0[i]*(t-b) - a*(b-t)*(2*r0[i]-r1[i]*e)*e \
							+ r0[i]*(a*(b-t)+1)*(e+1) \
							- r1[i]*(a*(b-t) + 1)*(e+1)*e) \
							/ SQR(e+1);			
			
			e = std::exp(a*(b+t));
			
			h.at(1,1) -= SQR(a)*(r0[i]+r1[i])*e/(std::exp(2*a*b) + std::exp(2*a*t) + 2*e);
		}
		h.at(1,0) = h.at(0,1);
	}
};




// [[Rcpp::export]]
Rcpp::List estimate_2pl_dich(const arma::vec& a_start, const arma::vec& b_start, 
						const arma::ivec& pni, const arma::ivec& pcni, const arma::ivec& pi, const arma::ivec& px, 
						arma::vec& theta, const double mu=0, const double sigma=1)
{
	const int nit = a_start.n_elem, nt = theta.n_elem, np = pni.n_elem;
	
	vec a(a_start.memptr(),nit), b(b_start.memptr(),nit);
	
	mat r0(nt,nit, fill::zeros), r1(nt,nit, fill::zeros);
	
	vec pars(2);
	vec thetabar(np,fill::zeros);
	
	double sumsig2;

	
	
	const int max_iter = 60;
	const double tol = 1e-8;
	
	for(int iter=0; iter<max_iter; iter++)
	{
		estep_2pl_dich(a, b, pni, pcni, pi, px, 
						theta, r0, r1, thetabar, sumsig2, mu, sigma);

		double loglikelihood=0;
		int nn=0;
		double maxdif_a=0, maxdif_b=0;
		for(int i=0; i<nit; i++)
		{		
			ll_2pl_dich f(r1.colptr(i), r0.colptr(i), theta.memptr(), nt);
			
			pars[0] = a[i];
			pars[1] = b[i];
			int itr=0;
			double ll=0;
			// need to build in overflow protection in logl voor item, otherwise can freeze in lnsrch
			dfpmin(pars, tol, itr, ll, f);
			
			maxdif_a = std::max(maxdif_a, std::abs(a[i]-pars[0]));
			maxdif_b = std::max(maxdif_b, std::abs(b[i]-pars[1]));
			
			a[i] = pars[0];
			b[i] = pars[1];
			loglikelihood += ll;
			nn+=itr;
		}
		printf("iter: %i, logl: %f, max a: %f, max b: %f\n",nn,loglikelihood,maxdif_a,maxdif_b);
		fflush(stdout);
	}
	return Rcpp::List::create(Named("a")=a, Named("b")=b, Named("thetabar") = thetabar, Named("sumsig2") = sumsig2);

}



// multiple group

// this is a reduced version of the estep
long double LL_2pl_dich(const vec& a, const vec& b, const ivec& pni, const ivec& pcni, const ivec& pi, const ivec& px, 
				const vec& theta, const vec& mu, const vec& sigma, const ivec& pgroup)

{
	const int nit = a.n_elem, nt = theta.n_elem, np = pni.n_elem, ng = mu.n_elem;
	mat itrace(nt,nit);
	
	mat posterior0(nt,ng);
	for(int g=0; g<ng; g++)
		posterior0.col(g) = gaussian_pts(mu[g],sigma[g],theta);
	
	for(int i=0; i<nit; i++)
		itrace.col(i) = 1/(1+exp(-a[i]*(theta-b[i])));
	
	long double ll=0;
	
#pragma omp parallel
	{
		vec posterior(nt);
# pragma omp for reduction(+:ll)
		for(int p=0; p<np;p++)
		{
			int g = pgroup[p];
			posterior = posterior0.col(g);
			
			for(int indx = pcni[p]; indx<pcni[p+1]; indx++)
			{
				if(px[indx] == 1)
					posterior %= itrace.col(pi[indx]);
				else
					posterior %= 1-itrace.col(pi[indx]);
			}	
			ll += std::log(accu(posterior));
		}
	}
	return ll;
}


void estep_2pl_dich(const vec& a, const vec& b, const ivec& pni, const ivec& pcni, const ivec& pi, const ivec& px, 
				const vec& theta, mat& r0, mat& r1, vec& thetabar, vec& sumtheta, vec& sumsig2, const vec& mu, const vec& sigma, const ivec& pgroup, double& ll)
{
	const int nit = a.n_elem, nt = theta.n_elem, np = pni.n_elem, ng = mu.n_elem;
	mat itrace(nt,nit);
	
	mat posterior0(nt,ng);
	for(int g=0; g<ng; g++)
		posterior0.col(g) = gaussian_pts(mu[g],sigma[g],theta);

	
	r0.zeros();
	r1.zeros();
	sumtheta.zeros();
	
	for(int i=0; i<nit; i++)
		itrace.col(i) = 1/(1+exp(-a[i]*(theta-b[i])));
		
	mat sigma2(nt, ng, fill::zeros);
	
	ll=0;
	
#pragma omp parallel
	{
		vec posterior(nt);
# pragma omp for reduction(+:r0,r1,sigma2, sumtheta,ll)
		for(int p=0; p<np;p++)
		{
			int g = pgroup[p];
			posterior = posterior0.col(g);
			
			for(int indx = pcni[p]; indx<pcni[p+1]; indx++)
			{
				if(px[indx] == 1)
					posterior %= itrace.col(pi[indx]);
				else
					posterior %= 1-itrace.col(pi[indx]);
			}	
			double sp = accu(posterior);
			// LL according to Bock/Aitkin 1981 eq (5) and (6), omitting constant C casue I don't know what C is
			ll += std::log(sp); 
			posterior = posterior / sp;
			sumtheta[g] += thetabar[p] = accu(posterior % theta);
			
			sigma2.col(g) += posterior;
			
			for(int indx = pcni[p]; indx<pcni[p+1]; indx++)
			{
				if(px[indx] == 1)
					r1.col(pi[indx]) += posterior;
				else
					r0.col(pi[indx]) += posterior;
			}		
		}
	}

	for(int g=0; g<ng;g++)
		sumsig2[g] = accu(sigma2.col(g) % square(theta));
}



// [[Rcpp::export]]
Rcpp::List estimate_2pl_dich_multigroup(const arma::vec& a_start, const arma::vec& b_start, 
						const arma::ivec& pni, const arma::ivec& pcni, const arma::ivec& pi, const arma::ivec& px, 
						arma::vec& theta, const arma::vec& mu_start, const arma::vec& sigma_start, const arma::ivec& gn, const arma::ivec& pgroup, const int ref_group=0)
{
	const int nit = a_start.n_elem, nt = theta.n_elem, np = pni.n_elem, ng=gn.n_elem;;
	
	vec a(a_start.memptr(),nit), b(b_start.memptr(),nit);
	
	mat r0(nt,nit, fill::zeros), r1(nt,nit, fill::zeros);	
	
	vec thetabar(np,fill::zeros);
	
	vec sigma = sigma_start, mu=mu_start;
	
	vec sum_theta(ng), sum_sigma2(ng);
	
	const int max_iter = 100;
	const double tol = 1e-8;
	int iter = 0;
	double ll;
	
	for(; iter<max_iter; iter++)
	{
		estep_2pl_dich(a, b, pni, pcni, pi, px, 
						theta, r0, r1, thetabar, sum_theta, sum_sigma2, mu, sigma, pgroup, ll);
		
		
		double maxdif_a=0, maxdif_b=0;
#pragma omp parallel for reduction(max: maxdif_a, maxdif_b)
		for(int i=0; i<nit; i++)
		{				
			ll_2pl_dich f(r1.colptr(i), r0.colptr(i), theta.memptr(), nt);
			vec pars(2);
			pars[0] = a[i];
			pars[1] = b[i];
			int itr=0;
			double ll_itm=0;
			
			// minimize, still need to tweak lnsrch a bit of replace by better line search algo
			dfpmin(pars, tol, itr, ll_itm, f);
			
			maxdif_a = std::max(maxdif_a, std::abs(a[i]-pars[0]));
			maxdif_b = std::max(maxdif_b, std::abs(b[i]-pars[1]));
			
			a[i] = pars[0];
			b[i] = pars[1];
		}
		
		for(int g=0;g<ng;g++)
		{			
			if(g==ref_group)
			{
				mu[g] = 0;
				sigma[g] = 1;
			}
			else
			{
				mu[g] = sum_theta[g]/gn[g];		
				sigma[g] = std::sqrt(sum_sigma2[g]/gn[g] - mu[g] * mu[g]);
			}
		}
		

		//printf("\r% 3i", iter);
		printf("iter: % 4i, logl: %.6f, max a: %.8f, max b: %.8f\n", iter, ll, maxdif_a, maxdif_b);
		fflush(stdout);
		
		
		if(maxdif_a < .0001 && maxdif_b < .0001)
			break;
		
	}
	// for not only items
	mat obs(nit*2,nit*2,fill::zeros);
	mat h(2,2);
	for(int i=0; i<nit; i++)
	{				
		ll_2pl_dich f(r1.colptr(i), r0.colptr(i), theta.memptr(), nt);
		vec pars(2);
		pars[0] = a[i];
		pars[1] = b[i];
		f.hess(pars,h);
		obs.at(i,i) = h.at(0,0);
		obs.at(nit+i,nit+i) = h.at(1,1);
		obs.at(i,nit+i) = h.at(1,0);
		obs.at(nit+i,i) = h.at(1,0);
	}

	
	
	printf("\n");
	fflush(stdout);
	
	return Rcpp::List::create(Named("a")=a, Named("b")=b, Named("thetabar") = thetabar, Named("mu") = mu, Named("sd") = sigma, 
									Named("LL") = ll, Named("niter")=iter,Named("obs")=obs); 
}

// [[Rcpp::export]]
arma::mat oakes(const arma::vec& a_fixed, const arma::vec& b_fixed, 
				const arma::ivec& pni, const arma::ivec& pcni, const arma::ivec& pi, const arma::ivec& px, 
				arma::vec& theta, const arma::vec& mu_fixed, const arma::vec& sigma_fixed, const arma::ivec& gn, const arma::ivec& pgroup)
{	
	const int nit = a_fixed.n_elem, nt = theta.n_elem, np = pni.n_elem, ng=gn.n_elem;
	
	mat a(nit,2),b(nit,2);
	mat mu(ng,2), sigma(ng,2);
	
	mat r0(nt,nit, fill::zeros), r1(nt,nit, fill::zeros);	
	
	vec thetabar(np,fill::zeros);
	
	
	vec sum_theta(ng), sum_sigma2(ng);
	
	const double tol = 1e-8;
	double ll;
	
	const double delta = 1e-05;
	vec signed_delta(2);
	signed_delta[0] = -delta;
	signed_delta[1] = delta;

	
	const int npar = 2 * (nit+ng);
	mat jacob(npar, npar);




	for(int j=0; j<npar; j++)
	{
		a.col(0) = a_fixed; a.col(1) = a_fixed; 
		b.col(0) = b_fixed; b.col(1) = b_fixed;
		mu.col(0) = mu_fixed;mu.col(1) = mu_fixed;
		sigma.col(0) = sigma_fixed;	sigma.col(1) = sigma_fixed;

		for(int d=0; d<=1; d++)
		{
			if(j<nit)
				a.at(j,d) += signed_delta[d]; 
			else if(j < 2*nit)
				b.at(j - nit, d) += signed_delta[d]; 
			else if(j < 2*nit+ng)
				mu.at(j-2*nit, d) += signed_delta[d]; 
			else
				sigma.at(j-2*nit-ng, d) += signed_delta[d]; 
			
			estep_2pl_dich(a.col(d), b.col(d), pni, pcni, pi, px, 
							theta, r0, r1, thetabar, sum_theta, sum_sigma2, mu.col(d), sigma.col(d), pgroup, ll);

#pragma omp parallel for
			for(int i=0; i<nit; i++)
			{				
				ll_2pl_dich f(r1.colptr(i), r0.colptr(i), theta.memptr(), nt);
				vec pars(2);
				pars[0] = a.at(i,d);
				pars[1] = b.at(i,d);
				int itr=0;
				double ll_itm=0;
				
				dfpmin(pars, tol, itr, ll_itm, f);
				
				a.at(i,d) = pars[0];
				b.at(i,d) = pars[1];
			}
			//printf("before groups\n");
			//fflush(stdout);
			for(int g=0;g<ng;g++)
			{			
				mu.at(g,d) = sum_theta[g]/gn[g];	
				sigma.at(g,d) = std::sqrt(sum_sigma2[g]/gn[g] - mu.at(g,d) * mu.at(g,d)); // directe afhankelijkheid, is dat wel ok?
			}
		}
		for(int i=0; i<nit; i++)
		{
			jacob.at(i,j) = (a.at(i,1) - a.at(i,0))/(2*delta);
			jacob.at(nit+i,j) = (b.at(i,1) - b.at(i,0))/(2*delta);
		}
		for(int g=0; g<ng; g++)
		{
			int p = 2 * nit + g; 
			jacob.at(p,j) = (mu.at(g,1) - mu.at(g,0))/(2*delta);
			jacob.at(p+ng,j) = (sigma.at(g,1) - sigma.at(g,0))/(2*delta);
		}		
	}
	
	return jacob;
}