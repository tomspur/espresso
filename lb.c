/* $Id$
 *
 * This file is part of the ESPResSo distribution (http://www.espresso.mpg.de).
 * It is therefore subject to the ESPResSo license agreement which you
 * accepted upon receiving the distribution and by which you are
 * legally bound while utilizing this file in any form or way.
 * There is NO WARRANTY, not even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. 
 * You should have received a copy of that license along with this
 * program; if not, refer to http://www.espresso.mpg.de/license.html
 * where its current version can be found, or write to
 * Max-Planck-Institute for Polymer Research, Theory Group, 
 * PO Box 3148, 55021 Mainz, Germany. 
 * Copyright (c) 2002-2007; all rights reserved unless otherwise stated.
 */

/** \file lb.c
 *
 * Lattice Boltzmann algorithm for hydrodynamic degrees of freedom.
 *
 * Includes fluctuating LB and coupling to MD particles via frictional 
 * momentum transfer.
 *
 */

#include <mpi.h>
#include <tcl.h>
#include <stdio.h>
#include "utils.h"
#include "parser.h"
#include "communication.h"
#include "grid.h"
#include "domain_decomposition.h"
#include "interaction_data.h"
#include "thermostat.h"
#include "lattice.h"
#include "halo.h"
#include "lb-d3q19.h"
#include "lb-boundaries.h"
#include "lb.h"

#ifdef LB

/** Flag indicating momentum exchange between particles and fluid */
int transfer_momentum = 0;

/** Struct holding the Lattice Boltzmann parameters */
LB_Parameters lbpar = { 0.0, 0.0, -1.0, -1.0, -1.0, 0.0, { 0.0, 0.0, 0.0} };

/** The DnQm model to be used. */
LB_Model lbmodel = { 19, d3q19_lattice, d3q19_coefficients, d3q19_w, NULL, 1./3. };
/* !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
 * ! MAKE SURE THAT D3Q19 is #undefined WHEN USING OTHER MODELS !
 * !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! */
/* doesn't work yet */
#ifndef D3Q19
#error The implementation only works for D3Q19 so far!
#endif

/** The underlying lattice structure */
Lattice lblattice = { {0,0,0}, {0,0,0}, 0, 0, 0, 0, -1.0, -1.0 };

/** Pointer to the velocity populations of the fluid nodes */
double **lbfluid[2];

/** Communicator for halo exchange between processors */
HaloCommunicator update_halo_comm = { 0, NULL };

/** \name Derived parameters */
/*@{*/
/** Flag indicating whether fluctuations are present. */
static int fluct;

/** relaxation rate of shear modes */
static double gamma_shear = 0.0;
/** relaxation rate of bulk modes */
static double gamma_bulk = 0.0;
/** amplitudes of the fluctuations of the modes */
static double lb_phi[19];
/** amplitude of the fluctuations in the viscous coupling */
static double lb_coupl_pref = 0.0;
/*@}*/

/** The number of velocities of the LB model.
 * This variable is used for convenience instead of having to type lbmodel.n_veloc everywhere. */
static int n_veloc;

/** Lattice spacing.
 * This variable is used for convenience instead of having to type lbpar.agrid everywhere. */
static double agrid;

/** Lattice Boltzmann time step
 * This variable is used for convenience instead of having to type lbpar.tau everywhere. */
static double tau;

/** measures the MD time since the last fluid update */
static double fluidstep=0.0;

#ifdef ADDITIONAL_CHECKS
/** counts the random numbers drawn for fluctuating LB and the coupling */
static int rancounter=0;
/** counts the occurences of negative populations due to fluctuations */
static int failcounter=0;
#endif

/***********************************************************************/

#ifdef ADDITIONAL_CHECKS
static int compare_buffers(double *buf1, double *buf2, int size) {
  int ret;
  if (memcmp(buf1,buf2,size)) {
    char *errtxt;
    errtxt = runtime_error(128);
    ERROR_SPRINTF(errtxt,"{102 Halo buffers are not identical} ");
    ret = 1;
  } else {
    ret = 0;
  }
  return ret;
}

/** Checks consistency of the halo regions (ADDITIONAL_CHECKS)
 * This function can be used as an additional check. It test whether the 
 * halo regions have been exchanged correctly. */
static void lb_check_halo_regions() {

  int i,x,y,z, index, s_node, r_node, count=n_veloc;
  double *s_buffer, *r_buffer;
  MPI_Status status[2];

  r_buffer = malloc(count*sizeof(double));
  s_buffer = malloc(count*sizeof(double));

  if (PERIODIC(0)) {
    for (z=0;z<lblattice.halo_grid[2];++z) {
      for (y=0;y<lblattice.halo_grid[1];++y) {

	index  = get_linear_index(0,y,z,lblattice.halo_grid);
	for (i=0;i<n_veloc;i++) s_buffer[i] = lbfluid[0][i][index];

	s_node = node_neighbors[1];
	r_node = node_neighbors[0];
	if (n_nodes > 1) {
	  MPI_Sendrecv(s_buffer, count, MPI_DOUBLE, r_node, REQ_HALO_CHECK,
		       r_buffer, count, MPI_DOUBLE, s_node, REQ_HALO_CHECK,
		       MPI_COMM_WORLD, status);
	  index = get_linear_index(lblattice.grid[0],y,z,lblattice.halo_grid);
	  for (i=0;i<n_veloc;i++) s_buffer[i] = lbfluid[0][i][index];
	  compare_buffers(s_buffer,r_buffer,count*sizeof(double));
	} else {
	  index = get_linear_index(lblattice.grid[0],y,z,lblattice.halo_grid);
	  for (i=0;i<n_veloc;i++) r_buffer[i] = lbfluid[0][i][index];
	  if (compare_buffers(s_buffer,r_buffer,count*sizeof(double))) {
	    fprintf(stderr,"buffers differ in dir=%d at index=%d y=%d z=%d\n",0,index,y,z);
	  }
	}

	index = get_linear_index(lblattice.grid[0]+1,y,z,lblattice.halo_grid); 
	for (i=0;i<n_veloc;i++) s_buffer[i] = lbfluid[0][i][index];

	s_node = node_neighbors[0];
	r_node = node_neighbors[1];
	if (n_nodes > 1) {
	  MPI_Sendrecv(s_buffer, count, MPI_DOUBLE, r_node, REQ_HALO_CHECK,
		       r_buffer, count, MPI_DOUBLE, s_node, REQ_HALO_CHECK,
		       MPI_COMM_WORLD, status);
	  index = get_linear_index(1,y,z,lblattice.halo_grid);
	  for (i=0;i<n_veloc;i++) s_buffer[i] = lbfluid[0][i][index];
	  compare_buffers(s_buffer,r_buffer,count*sizeof(double));
	} else {
	  index = get_linear_index(1,y,z,lblattice.halo_grid);
	  for (i=0;i<n_veloc;i++) r_buffer[i] = lbfluid[0][i][index];
	  if (compare_buffers(s_buffer,r_buffer,count*sizeof(double))) {
	    fprintf(stderr,"buffers differ in dir=%d at index=%d y=%d z=%d\n",0,index,y,z);	  
	  }
	}

      }      
    }
  }

  if (PERIODIC(1)) {
    for (z=0;z<lblattice.halo_grid[2];++z) {
      for (x=0;x<lblattice.halo_grid[0];++x) {

	index = get_linear_index(x,0,z,lblattice.halo_grid);
	for (i=0;i<n_veloc;i++) s_buffer[i] = lbfluid[0][i][index];

	s_node = node_neighbors[3];
	r_node = node_neighbors[2];
	if (n_nodes > 1) {
	  MPI_Sendrecv(s_buffer, count, MPI_DOUBLE, r_node, REQ_HALO_CHECK,
		       r_buffer, count, MPI_DOUBLE, s_node, REQ_HALO_CHECK,
		       MPI_COMM_WORLD, status);
	  index = get_linear_index(x,lblattice.grid[1],z,lblattice.halo_grid);
	  for (i=0;i<n_veloc;i++) s_buffer[i] = lbfluid[0][i][index];
	  compare_buffers(s_buffer,r_buffer,count*sizeof(double));
	} else {
	  index = get_linear_index(x,lblattice.grid[1],z,lblattice.halo_grid);
	  for (i=0;i<n_veloc;i++) r_buffer[i] = lbfluid[0][i][index];
	  if (compare_buffers(s_buffer,r_buffer,count*sizeof(double))) {
	    fprintf(stderr,"buffers differ in dir=%d at index=%d x=%d z=%d\n",1,index,x,z);
	  }
	}

      }
      for (x=0;x<lblattice.halo_grid[0];++x) {

	index = get_linear_index(x,lblattice.grid[1]+1,z,lblattice.halo_grid);
	for (i=0;i<n_veloc;i++) s_buffer[i] = lbfluid[0][i][index];

	s_node = node_neighbors[2];
	r_node = node_neighbors[3];
	if (n_nodes > 1) {
	  MPI_Sendrecv(s_buffer, count, MPI_DOUBLE, r_node, REQ_HALO_CHECK,
		       r_buffer, count, MPI_DOUBLE, s_node, REQ_HALO_CHECK,
		       MPI_COMM_WORLD, status);
	  index = get_linear_index(x,1,z,lblattice.halo_grid);
	  for (i=0;i<n_veloc;i++) s_buffer[i] = lbfluid[0][i][index];
	  compare_buffers(s_buffer,r_buffer,count*sizeof(double));
	} else {
	  index = get_linear_index(x,1,z,lblattice.halo_grid);
	  for (i=0;i<n_veloc;i++) r_buffer[i] = lbfluid[0][i][index];
	  if (compare_buffers(s_buffer,r_buffer,count*sizeof(double))) {
	    fprintf(stderr,"buffers differ in dir=%d at index=%d x=%d z=%d\n",1,index,x,z);
	  }
	}

      }
    }
  }

  if (PERIODIC(2)) {
    for (y=0;y<lblattice.halo_grid[1];++y) {
      for (x=0;x<lblattice.halo_grid[0];++x) {

	index = get_linear_index(x,y,0,lblattice.halo_grid);
	for (i=0;i<n_veloc;i++) s_buffer[i] = lbfluid[0][i][index];

	s_node = node_neighbors[5];
	r_node = node_neighbors[4];
	if (n_nodes > 1) {
	  MPI_Sendrecv(s_buffer, count, MPI_DOUBLE, r_node, REQ_HALO_CHECK,
		       r_buffer, count, MPI_DOUBLE, s_node, REQ_HALO_CHECK,
		       MPI_COMM_WORLD, status);
	  index = get_linear_index(x,y,lblattice.grid[2],lblattice.halo_grid);
	  for (i=0;i<n_veloc;i++) s_buffer[i] = lbfluid[0][i][index];
	  compare_buffers(s_buffer,r_buffer,count*sizeof(double));
	} else {
	  index = get_linear_index(x,y,lblattice.grid[2],lblattice.halo_grid);
	  for (i=0;i<n_veloc;i++) r_buffer[i] = lbfluid[0][i][index];
	  if (compare_buffers(s_buffer,r_buffer,count*sizeof(double))) {
	    fprintf(stderr,"buffers differ in dir=%d at index=%d x=%d y=%d z=%d\n",2,index,x,y,lblattice.grid[2]);  
	  }
	}

      }
    }
    for (y=0;y<lblattice.halo_grid[1];++y) {
      for (x=0;x<lblattice.halo_grid[0];++x) {

	index = get_linear_index(x,y,lblattice.grid[2]+1,lblattice.halo_grid);
	for (i=0;i<n_veloc;i++) s_buffer[i] = lbfluid[0][i][index];

	s_node = node_neighbors[4];
	r_node = node_neighbors[5];
	if (n_nodes > 1) {
	  MPI_Sendrecv(s_buffer, count, MPI_DOUBLE, r_node, REQ_HALO_CHECK,
		       r_buffer, count, MPI_DOUBLE, s_node, REQ_HALO_CHECK,
		       MPI_COMM_WORLD, status);
	  index = get_linear_index(x,y,1,lblattice.halo_grid);
	  for (i=0;i<n_veloc;i++) s_buffer[i] = lbfluid[0][i][index];
	  compare_buffers(s_buffer,r_buffer,count*sizeof(double));
	} else {
	  index = get_linear_index(x,y,1,lblattice.halo_grid);
	  for (i=0;i<n_veloc;i++) r_buffer[i] = lbfluid[0][i][index];
	  if(compare_buffers(s_buffer,r_buffer,count*sizeof(double))) {
	    fprintf(stderr,"buffers differ in dir=%d at index=%d x=%d y=%d\n",2,index,x,y);
	  }
	}
      
      }
    }
  }

  free(r_buffer);
  free(s_buffer);

  //if (check_runtime_errors());
  //else fprintf(stderr,"halo check successful\n");

}
#endif /* ADDITIONAL_CHECKS */

#ifdef ADDITIONAL_CHECKS
MDINLINE void lb_lattice_sum() {
    int i,a,b,c,d,e,f;
    double (*w) = lbmodel.w;
    double (*v)[3] = lbmodel.c;
    double sum;
    int count=0;
    
    for (a=0; a<3; a++)
	for (b=0; b<3; b++)
	    for (c=0; c<3; c++)
		for (d=0; d<3; d++)
		    for (e=0; e<3; e++)
			for (f=0; f<3; f++) {
			    sum = 0.0;
			    for (i=0; i<n_veloc; ++i) {
				sum += w[i]*v[i][a]*v[i][b]*v[i][c]*v[i][d]*v[i][e]*v[i][f];
			    }
			    if (sum!=0.0) { count++; fprintf(stderr,"(%d,%d,%d,%d,%d,%d) %f\n",a,b,c,d,e,f,sum/SQR(lbmodel.c_sound_sq)); }
			}
    fprintf(stderr,"%d non-null entries\n",count);

}
#endif

#ifdef ADDITIONAL_CHECKS
MDINLINE void lb_check_mode_transformation(int index, double *mode) {

  /* check if what I think is right */

  int i;
  double *w = lbmodel.w;
  double (*e)[19] = d3q19_modebase;
  double sum_n=0.0, sum_m=0.0;
  double n_eq[19];
  double m_eq[19];
  double avg_rho = lbpar.rho;
  double (*c)[3] = lbmodel.c;

  m_eq[0] = mode[0];
  m_eq[1] = mode[1];
  m_eq[2] = mode[2];
  m_eq[3] = mode[3];

  double rho = mode[0] + avg_rho;
  double *j  = mode+1;

  /* equilibrium part of the stress modes */
  /* remember that the modes have (\todo not?) been normalized! */
  m_eq[4] = /*1./6.*/scalar(j,j)/rho;
  m_eq[5] = /*1./4.*/(SQR(j[0])-SQR(j[1]))/rho;
  m_eq[6] = /*1./12.*/(scalar(j,j) - 3.0*SQR(j[2]))/rho;
  m_eq[7] = j[0]*j[1]/rho;
  m_eq[8] = j[0]*j[2]/rho;
  m_eq[9] = j[1]*j[2]/rho;

  for (i=10;i<n_veloc;i++) {
    m_eq[i] = 0.0;
  }

  for (i=0;i<n_veloc;i++) {
    n_eq[i] = w[i]*((rho-avg_rho) + 3.*scalar(j,c[i]) + 9./2.*SQR(scalar(j,c[i]))/rho - 3./2.*scalar(j,j)/rho);
  } 

  for (i=0;i<n_veloc;i++) {
    sum_n += SQR(lbfluid[0][i][index]-n_eq[i])/w[i];
    sum_m += SQR(mode[i]-m_eq[i])/e[19][i];
  }

  if (fabs(sum_n-sum_m)>ROUND_ERROR_PREC) {    
    fprintf(stderr,"Attention: sum_n=%f sum_m=%f %e\n",sum_n,sum_m,fabs(sum_n-sum_m));
  }

}

MDINLINE void lb_init_mode_transformation() {

#ifdef D3Q19
  int i, j, k, l;
  double *w     = lbmodel.w;
  double (*c)[3]= lbmodel.c;
  double b[19][19];
  double e[19][19];
  double proj, norm[19];

  /* construct polynomials from the discrete velocity vectors */
  for (i=0;i<n_veloc;i++) {
    b[0][i]  = 1;
    b[1][i]  = c[i][0];
    b[2][i]  = c[i][1];
    b[3][i]  = c[i][2];
    b[4][i]  = scalar(c[i],c[i]);
    b[5][i]  = c[i][0]*c[i][0]-c[i][1]*c[i][1];
    b[6][i]  = scalar(c[i],c[i])-3*c[i][2]*c[i][2];
    //b[5][i]  = 3*c[i][0]*c[i][0]-scalar(c[i],c[i]);
    //b[6][i]  = c[i][1]*c[i][1]-c[i][2]*c[i][2];
    b[7][i]  = c[i][0]*c[i][1];
    b[8][i]  = c[i][0]*c[i][2];
    b[9][i]  = c[i][1]*c[i][2];
    b[10][i] = 3*scalar(c[i],c[i])*c[i][0];
    b[11][i] = 3*scalar(c[i],c[i])*c[i][1];
    b[12][i] = 3*scalar(c[i],c[i])*c[i][2];
    b[13][i] = (c[i][1]*c[i][1]-c[i][2]*c[i][2])*c[i][0];
    b[14][i] = (c[i][0]*c[i][0]-c[i][2]*c[i][2])*c[i][1];
    b[15][i] = (c[i][0]*c[i][0]-c[i][1]*c[i][1])*c[i][2];
    b[16][i] = 3*scalar(c[i],c[i])*scalar(c[i],c[i]);
    b[17][i] = 2*scalar(c[i],c[i])*b[5][i];
    b[18][i] = 2*scalar(c[i],c[i])*b[6][i];
  }

  /* Gram-Schmidt orthogonalization procedure */
  for (j=0;j<n_veloc;j++) {
    for (i=0;i<n_veloc;i++) e[j][i] = b[j][i];
    for (k=0;k<j;k++) {
      proj = 0.0;
      for (l=0;l<n_veloc;l++) {
	proj += w[l]*e[k][l]*b[j][l];
      }
      for (i=0;i<n_veloc;i++) e[j][i] -= proj/norm[k]*e[k][i];
    }
    norm[j] = 0.0;
    for (i=0;i<n_veloc;i++) norm[j] += w[i]*SQR(e[j][i]);
  }
  
  fprintf(stderr,"e[%d][%d] = {\n",n_veloc,n_veloc);
  for (i=0;i<n_veloc;i++) {
    fprintf(stderr,"{ % .1f",e[i][0]);
    for (j=1;j<n_veloc;j++) {
      fprintf(stderr,", % .1f",e[i][j]);
    }
    fprintf(stderr," } %.2f\n",norm[i]);
  }
  fprintf(stderr,"};\n");

#else
  int i, j, k, l;
  double b[9][9];
  double e[9][9];
  double proj, norm[9];

  double c[9][2] = { { 0, 0 },
		     { 1, 0 },
		     {-1, 0 },
                     { 0, 1 },
                     { 0,-1 },
		     { 1, 1 },
		     {-1,-1 },
		     { 1,-1 },
		     {-1, 1 } };

  double w[9] = { 4./9, 1./9, 1./9, 1./9, 1./9, 1./36, 1./36, 1./36, 1./36 };

  n_veloc = 9;

  /* construct polynomials from the discrete velocity vectors */
  for (i=0;i<n_veloc;i++) {
    b[0][i] = 1;
    b[1][i] = c[i][0];
    b[2][i] = c[i][1];
    b[3][i] = 3*(SQR(c[i][0]) + SQR(c[i][1]));
    b[4][i] = c[i][0]*c[i][0]-c[i][1]*c[i][1];
    b[5][i] = c[i][0]*c[i][1];
    b[6][i] = 3*(SQR(c[i][0])+SQR(c[i][1]))*c[i][0];
    b[7][i] = 3*(SQR(c[i][0])+SQR(c[i][1]))*c[i][1];
    b[8][i] = (b[3][i]-5)*b[3][i]/2;
  }

  /* Gram-Schmidt orthogonalization procedure */
  for (j=0;j<n_veloc;j++) {
    for (i=0;i<n_veloc;i++) e[j][i] = b[j][i];
    for (k=0;k<j;k++) {
      proj = 0.0;
      for (l=0;l<n_veloc;l++) {
	proj += w[l]*e[k][l]*b[j][l];
      }
      for (i=0;i<n_veloc;i++) e[j][i] -= proj/norm[k]*e[k][i];
    }
    norm[j] = 0.0;
    for (i=0;i<n_veloc;i++) norm[j] += w[i]*SQR(e[j][i]);
  }
  
  fprintf(stderr,"e[%d][%d] = {\n",n_veloc,n_veloc);
  for (i=0;i<n_veloc;i++) {
    fprintf(stderr,"{ % .1f",e[i][0]);
    for (j=1;j<n_veloc;j++) {
      fprintf(stderr,", % .1f",e[i][j]);
    }
    fprintf(stderr," } %.2f\n",norm[i]);
  }
  fprintf(stderr,"};\n");

#endif

}
#endif /* ADDITIONAL_CHECKS */

#ifdef ADDITIONAL_CHECKS
/** Check for negative populations.  
 *
 * Checks for negative populations and increases failcounter for each
 * occurence.
 *
 * @param  local_node Pointer to the local lattice site (Input).
 * @return Number of negative populations on the local lattice site.
 */
MDINLINE int lb_check_negative_n(int index) {
  int i, localfails=0;

  for (i=0; i<n_veloc; i++) {
    if (lbfluid[0][i][index]+lbmodel.coeff[i][0]*lbpar.rho < 0.0) {
      ++localfails;
      ++failcounter;
      fprintf(stderr,"%d: Negative population n[%d]=%le (failcounter=%d, rancounter=%d).\n   Check your parameters if this occurs too often!\n",this_node,i,lbmodel.coeff[i][0]*lbpar.rho+lbfluid[0][i][index],failcounter,rancounter);
      break;
   }
  }

  return localfails;
}
#endif /* ADDITIONAL_CHECKS */

/***********************************************************************/

/** Performs basic sanity checks. */
static int lb_sanity_checks() {

  char *errtxt;
  int ret = 0;

    if (cell_structure.type != CELL_STRUCTURE_DOMDEC) {
      errtxt = runtime_error(128);
      ERROR_SPRINTF(errtxt, "{103 LB requires domain-decomposition cellsystem} ");
      ret = -1;
    } 
    else if (dd.use_vList) {
      errtxt = runtime_error(128);
      ERROR_SPRINTF(errtxt, "{104 LB requires no Verlet Lists} ");
      ret = -1;
    }    

    return ret;

}

/***********************************************************************/

/** (Pre-)allocate memory for data structures */
void lb_pre_init() {
  lbfluid[0]    = malloc(2*lbmodel.n_veloc*sizeof(double *));
  lbfluid[0][0] = malloc(2*lblattice.halo_grid_volume*lbmodel.n_veloc*sizeof(double));
}

/** (Re-)allocate memory for the fluid and initialize pointers. */
static void lb_realloc_fluid() {
  int i;

  lbfluid[0]    = realloc(*lbfluid,2*lbmodel.n_veloc*sizeof(double *));
  lbfluid[0][0] = realloc(**lbfluid,2*lblattice.halo_grid_volume*lbmodel.n_veloc*sizeof(double));
  lbfluid[1]    = (double **)lbfluid[0] + lbmodel.n_veloc;
  lbfluid[1][0] = (double *)lbfluid[0][0] + lblattice.halo_grid_volume*lbmodel.n_veloc;

  for (i=0; i<lbmodel.n_veloc; ++i) {
    lbfluid[0][i] = lbfluid[0][0] + i*lblattice.halo_grid_volume;
    lbfluid[1][i] = lbfluid[1][0] + i*lblattice.halo_grid_volume;
  }

}

/** Sets up the structures for exchange of the halo regions.
 *  See also \ref halo.c */
static void lb_prepare_communication() {
    int i;
    HaloCommunicator comm = { 0, NULL };

    /* since the data layout is a structure of arrays, we have to
     * generate a communication for this structure: first we generate
     * the communication for one of the arrays (the 0-th velocity
     * population), then we replicate this communication for the other
     * velocity indices by constructing appropriate vector
     * datatypes */

    /* prepare the communication for a single velocity */
    prepare_halo_communication(&comm, &lblattice, FIELDTYPE_DOUBLE, MPI_DOUBLE);

    update_halo_comm.num = comm.num;
    update_halo_comm.halo_info = realloc(update_halo_comm.halo_info,comm.num*sizeof(HaloInfo));

    /* replicate the halo structure */
    for (i=0; i<comm.num; i++) {
      HaloInfo *hinfo = &(update_halo_comm.halo_info[i]);

      hinfo->source_node = comm.halo_info[i].source_node;
      hinfo->dest_node   = comm.halo_info[i].dest_node;
      hinfo->s_offset    = comm.halo_info[i].s_offset;
      hinfo->r_offset    = comm.halo_info[i].r_offset;
      hinfo->type        = comm.halo_info[i].type;

      /* generate the vector datatype for the structure of lattices we
       * have to use hvector here because the extent of the subtypes
       * does not span the full lattice and hence we cannot get the
       * correct vskip out of them */

      MPI_Aint extent;
      MPI_Type_extent(MPI_DOUBLE,&extent);      
      MPI_Type_hvector(lbmodel.n_veloc,1,lblattice.halo_grid_volume*extent,comm.halo_info[i].datatype,&hinfo->datatype);
      MPI_Type_commit(&hinfo->datatype);
      
      halo_create_field_hvector(lbmodel.n_veloc,1,lblattice.halo_grid_volume*sizeof(double),comm.halo_info[i].fieldtype,&hinfo->fieldtype);
    }      

    release_halo_communication(&comm);    
}

/** (Re-)initializes the fluid. */
void lb_reinit_parameters() {
  int i;

  n_veloc = lbmodel.n_veloc;

  agrid   = lbpar.agrid;
  tau     = lbpar.tau;

  /* Eq. (80) Duenweg, Schiller, Ladd, PRE 76(3):036704 (2007). */
  gamma_shear = 1. - 2./(6.*lbpar.viscosity*tau/(agrid*agrid)+1.);

  if (lbpar.bulk_viscosity > 0.0) {
    /* Eq. (81) Duenweg, Schiller, Ladd, PRE 76(3):036704 (2007). */
    gamma_bulk = 1. - 2./(9.*lbpar.bulk_viscosity*tau/(agrid*agrid)+1.);
  }

  if (temperature > 0.0) {  /* fluctuating hydrodynamics ? */

    fluct = 1;

    /* Eq. (51) Duenweg, Schiller, Ladd, PRE 76(3):036704 (2007).
     * Note that the modes are not normalized as in the paper here! */
    double mu = temperature/lbmodel.c_sound_sq*tau*tau/(agrid*agrid);
    double (*e)[19] = d3q19_modebase;
    for (i=0; i<3; i++) lb_phi[i] = 0.0;
    lb_phi[4] = sqrt(mu*e[19][4]*(1.-SQR(gamma_bulk)));
    for (i=5; i<10; i++) lb_phi[i] = sqrt(mu*e[19][i]*(1.-SQR(gamma_shear)));
    for (i=10; i<n_veloc; i++) lb_phi[i] = sqrt(mu*e[19][i]);
 
    LB_TRACE(fprintf(stderr,"%d: gamma_shear=%f gamma_bulk=%f shear_fluct=%f bulk_fluct=%f mu=%f\n",this_node,gamma_shear,gamma_bulk,lb_phi[9],lb_phi[4],mu));

    /* lb_coupl_pref is stored in MD units (force)
     * Eq. (16) Ahlrichs and Duenweg, JCP 111(17):8225 (1999).
     * The factor 12 comes from the fact that we use random numbers
     * from -0.5 to 0.5 (equally distributed) which have variance 1/12.
     * time_step comes from the discretization.
     */
    lb_coupl_pref = sqrt(12.*2.*lbpar.friction*temperature/time_step); 

    LB_TRACE(fprintf(stderr,"%d: lb_coupl_pref=%f (temp=%f, friction=%f, time_step=%f)\n",this_node,lb_coupl_pref,temperature,lbpar.friction,time_step));

  } else {
    /* no fluctuations at zero temperature */
    fluct = 0;
    for (i=0;i<n_veloc;i++) lb_phi[i] = 0.0;
    lb_coupl_pref = 0.0;
  }

}

/** (Re-)initializes the fluid according to the given value of rho. */
void lb_reinit_fluid() {

    int index;

    /* default values for fields in lattice units */
    double rho = lbpar.rho/(agrid*agrid*agrid) ;
    double v[3] = { 0., 0., 0. };
    double pi[6] = { rho*lbmodel.c_sound_sq, 0., rho*lbmodel.c_sound_sq, 0., 0., rho*lbmodel.c_sound_sq };

    for (index=0; index<lblattice.halo_grid_volume; index++) {

#ifdef CONSTRAINTS
      if (lbfluid[k].boundary==0) {
	lb_set_local_fields(index,rho,v,pi);
      } else {
        lb_set_boundary_fields(index,rho,v,pi);
      }
#else
      lb_calc_n_equilibrium(index,rho,v,pi);
#endif

    }

}

/** Performs a full initialization of
 *  the Lattice Boltzmann system. All derived parameters
 *  and the fluid are reset to their default values. */
void lb_init() {

  if (lb_sanity_checks()) return;

  /* initialize the local lattice domain */
  init_lattice(&lblattice,lbpar.agrid,lbpar.tau);  

  if (check_runtime_errors()) return;

  /* allocate memory for data structures */
  lb_realloc_fluid();

  /* prepare the halo communication */
  lb_prepare_communication();

  /* initialize derived parameters */
  lb_reinit_parameters();

#ifdef CONSTRAINTS
  /* setup boundaries of constraints */
  lb_init_constraints();
#endif

  /* setup the initial particle velocity distribution */
  lb_reinit_fluid();

}

/** Release the fluid. */
MDINLINE void lb_release_fluid() {
  free(lbfluid[0][0]);
  free(lbfluid[0]);
}

/** Release fluid and communication. */
void lb_release() {
  
  lb_release_fluid();

  release_halo_communication(&update_halo_comm);

}

/***********************************************************************/
/** \name Mapping between hydrodynamic fields and particle populations */
/***********************************************************************/
/*@{*/

/** Calculate local populations from hydrodynamic fields.
 *
 * The mapping is given in terms of the equilibrium distribution.
 *
 * Eq. (2.15) Ladd, J. Fluid Mech. 271, 295-309 (1994)
 * Eq. (4) in Berk Usta, Ladd and Butler, JCP 122, 094902 (2005)
 *
 * @param local_node Pointer to the local lattice site (Input).
 * @param trace      Trace of the local stress tensor (Input).
 * @param trace_eq   Trace of equilibriumd part of local stress tensor (Input).
 */
void lb_calc_n_equilibrium(const int index, const double rho, const double *v, double *pi) {

  const double rhoc_sq = rho*lbmodel.c_sound_sq;
  const double avg_rho = lbpar.rho/(agrid*agrid*agrid);

  double local_rho, local_j[3], *local_pi, trace;

  local_rho  = rho;

  local_j[0] = rho * v[0];
  local_j[1] = rho * v[1];
  local_j[2] = rho * v[2];

  local_pi = pi;

  /* reduce the pressure tensor to the part needed here */
  local_pi[0] -= rhoc_sq;
  local_pi[2] -= rhoc_sq;
  local_pi[5] -= rhoc_sq;

  trace = local_pi[0] + local_pi[2] + local_pi[5];

#ifdef D3Q19
  double rho_times_coeff;
  double tmp1,tmp2;

  /* update the q=0 sublattice */
  lbfluid[0][0][index] = 1./3. * (local_rho-avg_rho) - 1./2.*trace;

  /* update the q=1 sublattice */
  rho_times_coeff = 1./18. * (local_rho-avg_rho);

  lbfluid[0][1][index] = rho_times_coeff + 1./6.*local_j[0] + 1./4.*local_pi[0] - 1./12.*trace;
  lbfluid[0][2][index] = rho_times_coeff - 1./6.*local_j[0] + 1./4.*local_pi[0] - 1./12.*trace;
  lbfluid[0][3][index] = rho_times_coeff + 1./6.*local_j[1] + 1./4.*local_pi[2] - 1./12.*trace;
  lbfluid[0][4][index] = rho_times_coeff - 1./6.*local_j[1] + 1./4.*local_pi[2] - 1./12.*trace;
  lbfluid[0][5][index] = rho_times_coeff + 1./6.*local_j[2] + 1./4.*local_pi[5] - 1./12.*trace;
  lbfluid[0][6][index] = rho_times_coeff - 1./6.*local_j[2] + 1./4.*local_pi[5] - 1./12.*trace;

  /* update the q=2 sublattice */
  rho_times_coeff = 1./36. * (local_rho-avg_rho);

  tmp1 = local_pi[0] + local_pi[2];
  tmp2 = 2.0*local_pi[1];

  lbfluid[0][7][index]  = rho_times_coeff + 1./12.*(local_j[0]+local_j[1]) + 1./8.*(tmp1+tmp2) - 1./24.*trace;
  lbfluid[0][8][index]  = rho_times_coeff - 1./12.*(local_j[0]+local_j[1]) + 1./8.*(tmp1+tmp2) - 1./24.*trace;
  lbfluid[0][9][index]  = rho_times_coeff + 1./12.*(local_j[0]-local_j[1]) + 1./8.*(tmp1-tmp2) - 1./24.*trace;
  lbfluid[0][10][index] = rho_times_coeff - 1./12.*(local_j[0]-local_j[1]) + 1./8.*(tmp1-tmp2) - 1./24.*trace;

  tmp1 = local_pi[0] + local_pi[5];
  tmp2 = 2.0*local_pi[3];

  lbfluid[0][11][index] = rho_times_coeff + 1./12.*(local_j[0]+local_j[2]) + 1./8.*(tmp1+tmp2) - 1./24.*trace;
  lbfluid[0][12][index] = rho_times_coeff - 1./12.*(local_j[0]+local_j[2]) + 1./8.*(tmp1+tmp2) - 1./24.*trace;
  lbfluid[0][13][index] = rho_times_coeff + 1./12.*(local_j[0]-local_j[2]) + 1./8.*(tmp1-tmp2) - 1./24.*trace;
  lbfluid[0][14][index] = rho_times_coeff - 1./12.*(local_j[0]-local_j[2]) + 1./8.*(tmp1-tmp2) - 1./24.*trace;

  tmp1 = local_pi[2] + local_pi[5];
  tmp2 = 2.0*local_pi[4];

  lbfluid[0][15][index] = rho_times_coeff + 1./12.*(local_j[1]+local_j[2]) + 1./8.*(tmp1+tmp2) - 1./24.*trace;
  lbfluid[0][16][index] = rho_times_coeff - 1./12.*(local_j[1]+local_j[2]) + 1./8.*(tmp1+tmp2) - 1./24.*trace;
  lbfluid[0][17][index] = rho_times_coeff + 1./12.*(local_j[1]-local_j[2]) + 1./8.*(tmp1-tmp2) - 1./24.*trace;
  lbfluid[0][18][index] = rho_times_coeff - 1./12.*(local_j[1]-local_j[2]) + 1./8.*(tmp1-tmp2) - 1./24.*trace;

#else
  int i;
  double tmp=0.0;
  double (*c)[3] = lbmodel.c;
  double (*coeff)[4] = lbmodel.coeff;

  for (i=0;i<n_veloc;i++) {

    tmp = local_pi[0]*c[i][0]*c[i][0]
      + (2.0*local_pi[1]*c[i][0]+local_pi[2]*c[i][1])*c[i][1]
      + (2.0*(local_pi[3]*c[i][0]+local_pi[4]*c[i][1])+local_pi[5]*c[i][2])*c[i][2];

    lbfluid[0][i][index] =  coeff[i][0] * (*local_rho-avg_rho);
    lbfluid[0][i][index] += coeff[i][1] * scalar(local_j,c[i]);
    lbfluid[0][i][index] += coeff[i][2] * tmp;
    lbfluid[0][i][index] += coeff[i][3] * trace;

  }
#endif

  /* restore the pressure tensor to the full part */
  local_pi[0] += rhoc_sq;
  local_pi[2] += rhoc_sq;
  local_pi[5] += rhoc_sq;

}
  
/*@}*/

/** Streaming and calculation of modes (pull scheme) */
MDINLINE void lb_sc_calc_modes(int index, double *mode) {

  int yperiod = lblattice.halo_grid[0];
  int zperiod = lblattice.halo_grid[0]*lblattice.halo_grid[1];

  double n[19];
  n[0]  = lbfluid[0][0][index];
  n[1]  = lbfluid[0][1][index-1];
  n[2]  = lbfluid[0][2][index+1];
  n[3]  = lbfluid[0][3][index-yperiod];
  n[4]  = lbfluid[0][4][index+yperiod];
  n[5]  = lbfluid[0][5][index-zperiod];
  n[6]  = lbfluid[0][6][index+zperiod];
  n[7]  = lbfluid[0][7][index-(1+yperiod)];
  n[8]  = lbfluid[0][8][index+(1+yperiod)];
  n[9]  = lbfluid[0][9][index-(1-yperiod)];
  n[10] = lbfluid[0][10][index+(1-yperiod)];
  n[11] = lbfluid[0][11][index-(1+zperiod)];
  n[12] = lbfluid[0][12][index+(1+zperiod)];
  n[13] = lbfluid[0][13][index-(1-zperiod)];
  n[14] = lbfluid[0][14][index+(1-zperiod)];
  n[15] = lbfluid[0][15][index-(yperiod+zperiod)];
  n[16] = lbfluid[0][16][index+(yperiod+zperiod)];
  n[17] = lbfluid[0][17][index-(yperiod-zperiod)];
  n[18] = lbfluid[0][18][index+(yperiod-zperiod)];

  /* mass mode */
  mode[ 0] =   n[ 0] + n[ 1] + n[ 2] + n[ 3] + n[4] + n[5] + n[6]
             + n[ 7] + n[ 8] + n[ 9] + n[10]
             + n[11] + n[12] + n[13] + n[14]
             + n[15] + n[16] + n[17] + n[18];

  /* momentum modes */
  mode[ 1] =   n[ 1] - n[ 2] 
             + n[ 7] - n[ 8] + n[ 9] - n[10] + n[11] - n[12] + n[13] - n[14];
  mode[ 2] =   n[ 3] - n[ 4]
             + n[ 7] - n[ 8] - n[ 9] + n[10] + n[15] - n[16] + n[17] - n[18];
  mode[ 3] =   n[ 5] - n[ 6]
             + n[11] - n[12] - n[13] + n[14] + n[15] - n[16] - n[17] + n[18];

  /* stress modes */
  mode[ 4] = - n[ 0] 
             + n[ 7] + n[ 8] + n[ 9] + n[10] 
             + n[11] + n[12] + n[13] + n[14] 
             + n[15] + n[16] + n[17] + n[18];
  mode[ 5] =   n[ 1] + n[ 2] - n[ 3] - n[4]
             + n[11] + n[12] + n[13] + n[14] - n[15] - n[16] - n[17] - n[18];
  mode[ 6] =   n[ 1] + n[ 2] + n[ 3] + n[ 4] 
             - n[11] - n[12] - n[13] - n[14] - n[15] - n[16] - n[17] - n[18]
             - 2.*(n[5] + n[6] - n[7] - n[8] - n[9] - n[10]);
  mode[ 7] =   n[ 7] + n[ 8] - n[ 9] - n[10];
  mode[ 8] =   n[11] + n[12] - n[13] - n[14];
  mode[ 9] =   n[15] + n[16] - n[17] - n[18];

  /* ghost modes */
  mode[10] = 2.*(n[2] - n[1]) 
             + n[7] - n[8] + n[9] - n[10] + n[11] - n[12] + n[13] - n[14];
  mode[11] = 2.*(n[4] - n[3])
             + n[7] - n[8] - n[9] + n[10] + n[15] - n[16] + n[17] - n[18];
  mode[12] = 2.*(n[6] - n[5])
             + n[11] - n[12] - n[13] + n[14] + n[15] - n[16] - n[17] + n[18];
  mode[13] =   n[ 7] - n[ 8] + n[ 9] - n[10] - n[11] + n[12] - n[13] + n[14];
  mode[14] =   n[ 7] - n[ 8] - n[ 9] + n[10] - n[15] + n[16] - n[17] + n[18];
  mode[15] =   n[11] - n[12] - n[13] + n[14] - n[15] + n[16] + n[17] - n[18];
  mode[16] =   n[ 0]
             + n[ 7] + n[ 8] + n[ 9] + n[10] 
             + n[11] + n[12] + n[13] + n[14] 
             + n[15] + n[16] + n[17] + n[18]
             - 2.*(n[1] + n[2] + n[3] + n[4] + n[5] + n[6]);
  mode[17] =   n[ 3] + n[ 4] - n[ 1] - n[ 2] 
             + n[11] + n[12] + n[13] + n[14] 
             - n[15] - n[16] - n[17] - n[18];
  mode[18] = - n[ 1] - n[ 2] - n[ 3] - n[ 4] 
             - n[11] - n[12] - n[13] - n[14] - n[15] - n[16] - n[17] - n[18]
             + 2.*(n[5] + n[6] + n[7] + n[8] + n[9] + n[10]);

}

MDINLINE void lb_relax_modes(double *mode) {

  double rho, j[3], pi_eq[6];

  /* re-construct the real density 
   * remember that the populations are stored as differences to their
   * equilibrium value */
  rho = mode[0] + lbpar.rho;

  j[0] = mode[1];
  j[1] = mode[2];
  j[2] = mode[3];

#ifdef EXTERNAL_FORCES
  /* if external forces are present, the momentum density is
   * redefined to inlcude one half-step of the force action.
   * See the Chapman-Enskog expansion in [Ladd & Verberg]. */
  j[0] += 0.5*lbpar.ext_force[0];
  j[1] += 0.5*lbpar.ext_force[1];
  j[2] += 0.5*lbpar.ext_force[2];
#endif

  /* equilibrium part of the stress modes */
  pi_eq[0] = scalar(j,j)/rho;
  pi_eq[1] = (SQR(j[0])-SQR(j[1]))/rho;
  pi_eq[2] = (scalar(j,j) - 3.0*SQR(j[2]))/rho;
  pi_eq[3] = j[0]*j[1]/rho;
  pi_eq[4] = j[0]*j[2]/rho;
  pi_eq[5] = j[1]*j[2]/rho;

  /* relax the stress modes */  
  mode[4] = pi_eq[0] + gamma_bulk*(mode[4] - pi_eq[0]);
  mode[5] = pi_eq[1] + gamma_shear*(mode[5] - pi_eq[1]);
  mode[6] = pi_eq[2] + gamma_shear*(mode[6] - pi_eq[2]);
  mode[7] = pi_eq[3] + gamma_shear*(mode[7] - pi_eq[3]);
  mode[8] = pi_eq[4] + gamma_shear*(mode[8] - pi_eq[4]);
  mode[9] = pi_eq[5] + gamma_shear*(mode[9] - pi_eq[5]);

  /* relax the ghost modes (project them out) */
  /* ghost modes have no equilibrium part due to orthogonality */
  mode[10] = 0.0;
  mode[11] = 0.0;
  mode[12] = 0.0;
  mode[13] = 0.0;
  mode[14] = 0.0;
  mode[15] = 0.0;
  mode[16] = 0.0;
  mode[17] = 0.0;
  mode[18] = 0.0;

}

MDINLINE void lb_thermalize_modes(double *mode) {
    double rootrho = sqrt(mode[0]+lbpar.rho);
    double fluct[6];

    /* stress modes */
    mode[4] += (fluct[0] = rootrho*lb_phi[4]*gaussian_random());
    mode[5] += (fluct[1] = rootrho*lb_phi[5]*gaussian_random());
    mode[6] += (fluct[2] = rootrho*lb_phi[6]*gaussian_random());
    mode[7] += (fluct[3] = rootrho*lb_phi[7]*gaussian_random());
    mode[8] += (fluct[4] = rootrho*lb_phi[8]*gaussian_random());
    mode[9] += (fluct[5] = rootrho*lb_phi[9]*gaussian_random());
    //if (node == &lbfluid[lblattice.halo_offset]) {
    //  fprintf(stderr,"%f %f %f %f %f %f\n",fluct[0],fluct[1],fluct[2],fluct[3],fluct[4],fluct[5]);
    //}
    
    /* ghost modes */
    mode[10] += rootrho*lb_phi[10]*gaussian_random();
    mode[11] += rootrho*lb_phi[11]*gaussian_random();
    mode[12] += rootrho*lb_phi[12]*gaussian_random();
    mode[13] += rootrho*lb_phi[13]*gaussian_random();
    mode[14] += rootrho*lb_phi[14]*gaussian_random();
    mode[15] += rootrho*lb_phi[15]*gaussian_random();
    mode[16] += rootrho*lb_phi[16]*gaussian_random();
    mode[17] += rootrho*lb_phi[17]*gaussian_random();
    mode[18] += rootrho*lb_phi[18]*gaussian_random();

#ifdef ADDITIONAL_CHECKS
    rancounter += 15;
#endif
}

MDINLINE void lb_external_forces(double* mode) {

#ifdef EXTERNAL_FORCES
  double rho, f[3], u[3], C[6];
  
  f[0] = lbpar.ext_force[0];
  f[1] = lbpar.ext_force[1];
  f[2] = lbpar.ext_force[2];

  rho = mode[0] + lbpar.rho;

  /* hydrodynamic momentum density is redefined when external forces present */
  u[0] = (mode[1] + 0.5*f[0])/rho;
  u[1] = (mode[2] + 0.5*f[1])/rho;
  u[2] = (mode[3] + 0.5*f[2])/rho;

  C[0] = (1.+gamma_bulk)*u[0]*f[0] + 1./3.*(gamma_bulk-gamma_shear)*scalar(u,f);
  C[2] = (1.+gamma_bulk)*u[1]*f[1] + 1./3.*(gamma_bulk-gamma_shear)*scalar(u,f);
  C[5] = (1.+gamma_bulk)*u[2]*f[2] + 1./3.*(gamma_bulk-gamma_shear)*scalar(u,f);
  C[1] = 1./2.*(1.+gamma_shear)*(u[0]*f[1]+u[1]*f[0]);
  C[3] = 1./2.*(1.+gamma_shear)*(u[0]*f[2]+u[2]*f[0]);
  C[4] = 1./2.*(1.+gamma_shear)*(u[1]*f[2]+u[2]*f[1]);

  /* update momentum modes */
  mode[1] += f[0];
  mode[2] += f[1];
  mode[3] += f[2];

  /* update stress modes */
  mode[4] += C[0] + C[2] + C[5];
  mode[5] += 2.*C[0] - C[2] - C[5];
  mode[6] += C[2] - C[5];
  mode[7] += C[1];
  mode[8] += C[3];
  mode[9] += C[4];
#endif

}

MDINLINE void lb_calc_n_from_modes(int index, double *mode) {

  double *w = lbmodel.w;
  double m[19];
  int i;

#ifdef D3Q19
  double (*e)[19] = d3q19_modebase;

  /* normalization factors enter in the back transformation */
  for (i=0;i<n_veloc;i++) {
    m[i] = 1./e[19][i]*mode[i];
  }

  lbfluid[1][ 0][index] = m[0] - m[4] + m[16];
  lbfluid[1][ 1][index] = m[0] + m[1] + m[5] + m[6] - m[17] - m[18] - 2.*(m[10] + m[16]);
  lbfluid[1][ 2][index] = m[0] - m[1] + m[5] + m[6] - m[17] - m[18] + 2.*(m[10] - m[16]);
  lbfluid[1][ 3][index] = m[0] + m[2] - m[5] + m[6] + m[17] - m[18] - 2.*(m[11] + m[16]);
  lbfluid[1][ 4][index] = m[0] - m[2] - m[5] + m[6] + m[17] - m[18] + 2.*(m[11] - m[16]);
  lbfluid[1][ 5][index] = m[0] + m[3] - 2.*(m[6] + m[12] + m[16] - m[18]);
  lbfluid[1][ 6][index] = m[0] - m[3] - 2.*(m[6] - m[12] + m[16] - m[18]);
  lbfluid[1][ 7][index] = m[0] + m[ 1] + m[ 2] + m[ 4] + 2.*m[6]
        + m[7] + m[10] + m[11] + m[13] + m[14] + m[16] + 2.*m[18];
  lbfluid[1][ 8][index] = m[0] - m[ 1] - m[ 2] + m[ 4] + 2.*m[6]
        + m[7] - m[10] - m[11] - m[13] - m[14] + m[16] + 2.*m[18];
  lbfluid[1][ 9][index] = m[0] + m[ 1] - m[ 2] + m[ 4] + 2.*m[6]
        - m[7] + m[10] - m[11] + m[13] - m[14] + m[16] + 2.*m[18];
  lbfluid[1][10][index] = m[0] - m[ 1] + m[ 2] + m[ 4] + 2.*m[6]
        - m[7] - m[10] + m[11] - m[13] + m[14] + m[16] + 2.*m[18];
  lbfluid[1][11][index] = m[0] + m[ 1] + m[ 3] + m[ 4] + m[ 5] - m[ 6]
        + m[8] + m[10] + m[12] - m[13] + m[15] + m[16] + m[17] - m[18];
  lbfluid[1][12][index] = m[0] - m[ 1] - m[ 3] + m[ 4] + m[ 5] - m[ 6]
        + m[8] - m[10] - m[12] + m[13] - m[15] + m[16] + m[17] - m[18];
  lbfluid[1][13][index] = m[0] + m[ 1] - m[ 3] + m[ 4] + m[ 5] - m[ 6]
        - m[8] + m[10] - m[12] - m[13] - m[15] + m[16] + m[17] - m[18];
  lbfluid[1][14][index] = m[0] - m[ 1] + m[ 3] + m[ 4] + m[ 5] - m[ 6]
        - m[8] - m[10] + m[12] + m[13] + m[15] + m[16] + m[17] - m[18];
  lbfluid[1][15][index] = m[0] + m[ 2] + m[ 3] + m[ 4] - m[ 5] - m[ 6]
        + m[9] + m[11] + m[12] - m[14] - m[15] + m[16] - m[17] - m[18];
  lbfluid[1][16][index] = m[0] - m[ 2] - m[ 3] + m[ 4] - m[ 5] - m[ 6]
        + m[9] - m[11] - m[12] + m[14] + m[15] + m[16] - m[17] - m[18];
  lbfluid[1][17][index] = m[0] + m[ 2] - m[ 3] + m[ 4] - m[ 5] - m[ 6]
        - m[9] + m[11] - m[12] - m[14] + m[15] + m[16] - m[17] - m[18];
  lbfluid[1][18][index] = m[0] - m[ 2] + m[ 3] + m[ 4] - m[ 5] - m[ 6]
        - m[9] - m[11] + m[12] + m[14] - m[15] + m[16] - m[17] - m[18];

  /* weights enter in the back transformation */
  for (i=0;i<n_veloc;i++) {
    lbfluid[1][i][index] *= w[i];
  }

#else
  int j;
  double n2[n_veloc];
  for (i=0; i<n_veloc;i++) {
    n2[i] = 0.0;
    for (j=0;j<n_veloc;j++) {
      n2[i] += 1./e[19][j]*mode[j]*e[j][i];
    }
    n2[i] = w[i]*(n2[i]);
  }

  for (i=0;i<n_veloc;i++) {
    fprintf(stderr,"n[%d]=%f n2[%d]=%f\n",i,n[i],i,n2[i]);
  }
#endif

}

/** Streaming and collisions (pull scheme) */
MDINLINE void lb_stream_collide(int index) {

  double modes[19];

#ifdef CONSTRAINTS
  //if (!lbfluid[index].boundary) 
#endif
  {

    /* stream (pull) and calculate modes */
    lb_sc_calc_modes(index, modes);
  
    /* deterministic collisions */
    lb_relax_modes(modes);
    
    /* fluctuating hydrodynamics */
    if (fluct) lb_thermalize_modes(modes);
  
#ifdef EXTERNAL_FORCES
    /* external forces */
    lb_external_forces(modes);
#endif
    
    /* calculate new particle populations */
    lb_calc_n_from_modes(index, modes);

  }

}

/***********************************************************************/
/** \name Update step for the lattice Boltzmann fluid                  */
/***********************************************************************/
/*@{*/

/** Update the lattice Boltzmann fluid.  
 *
 * This function is called from the integrator. Since the time step
 * for the lattice dynamics can be coarser than the MD time step, we
 * monitor the time since the last lattice update.
 */
void lattice_boltzmann_update() {

  int x, y, z, index;
  double** tmp;

  fluidstep += time_step;

  if (fluidstep>=tau) {

    fluidstep=0.0;

    /* exchange halo regions */
    halo_communication(&update_halo_comm,**lbfluid);
#ifdef ADDITIONAL_CHECKS
    lb_check_halo_regions();
#endif

#ifdef CONSTRAINTS
    /* apply boundary conditions */
    lb_boundary_conditions();
#endif

    /* loop over all lattice cells (halo_excluded) */
    index = lblattice.halo_offset;
    for (z=1; z<=lblattice.grid[2]; z++) {
      for (y=1; y<=lblattice.grid[1]; y++) {
	for (x=1; x<=lblattice.grid[0]; x++) {
	  
	  lb_stream_collide(index);

	  ++index; /* next node */
	}

	index += 2; /* skip halo region */	
      }

      index += 2*lblattice.halo_grid[0]; /* skip halo region */
    }

    /* swap the pointers for old and new population fields */
    //fprintf(stderr,"swapping pointers\n");
    tmp = lbfluid[0];
    lbfluid[0] = lbfluid[1];
    lbfluid[1] = tmp;

  }
  
}

/***********************************************************************/
/** \name Coupling part */
/***********************************************************************/
/*@{*/

/** Transfer a certain amount of momentum to a elementray cell of fluid.
 * 
 * Eq. (14) Ahlrichs and Duenweg, JCP 111(17):8225 (1999).
 *
 * @param momentum   Momentum to be transfered to the fluid (lattice
 *                   units) (Input).
 * @param node_index Indices of the sites of the elementary lattice
 *                   cell (Input).
 * @param delta      Weights for the assignment to the single lattice
 *                   sites (Input).
 * @param badrandoms Flag/Counter for the occurrence negative
 *                   populations (Output).
 */
MDINLINE void lb_transfer_momentum(const double momentum[3], const int node_index[8], const double delta[6]) {

  int x, y, z, index;
  double delta_j[3];

  /* We don't need to save the local populations because 
   * we use a trick for their restoration:
   * We substract the old random force from the new one,
   * hence the previous change in the local populations
   * is automatically revoked during the recalculation.
   * Note that this makes it necessary to actually apply 
   * all changes and forbids to return immediately when negative
   * populations occur.
   */

  for (z=0;z<2;z++) {
    for (y=0;y<2;y++) {
      for (x=0;x<2;x++) {
	
	index = node_index[(z*2+y)*2+x];

	delta_j[0] = delta[3*x+0]*delta[3*y+1]*delta[3*z+2]*momentum[0];
	delta_j[1] = delta[3*x+0]*delta[3*y+1]*delta[3*z+2]*momentum[1];
	delta_j[2] = delta[3*x+0]*delta[3*y+1]*delta[3*z+2]*momentum[2];

#ifdef D3Q19
	lbfluid[0][1][index]  += + 1./6.*delta_j[0];
	lbfluid[0][2][index]  += - 1./6.*delta_j[0];
	lbfluid[0][3][index]  += + 1./6.*delta_j[1];
	lbfluid[0][4][index]  += - 1./6.*delta_j[1];
	lbfluid[0][5][index]  += + 1./6.*delta_j[2];
	lbfluid[0][6][index]  += - 1./6.*delta_j[2];
	lbfluid[0][7][index]  += + 1./12.*(delta_j[0]+delta_j[1]);
	lbfluid[0][8][index]  += - 1./12.*(delta_j[0]+delta_j[1]);
	lbfluid[0][9][index]  += + 1./12.*(delta_j[0]-delta_j[1]);
	lbfluid[0][10][index] += - 1./12.*(delta_j[0]-delta_j[1]);
	lbfluid[0][11][index] += + 1./12.*(delta_j[0]+delta_j[2]);
	lbfluid[0][12][index] += - 1./12.*(delta_j[0]+delta_j[2]);
	lbfluid[0][13][index] += + 1./12.*(delta_j[0]-delta_j[2]);
	lbfluid[0][14][index] += - 1./12.*(delta_j[0]-delta_j[2]);
	lbfluid[0][15][index] += + 1./12.*(delta_j[1]+delta_j[2]);
	lbfluid[0][16][index] += - 1./12.*(delta_j[1]+delta_j[2]);
	lbfluid[0][17][index] += + 1./12.*(delta_j[1]-delta_j[2]);
	lbfluid[0][18][index] += - 1./12.*(delta_j[1]-delta_j[2]);
#else
	int i;
	double (*c)[3] = lbmodel.c;
	double (*coeff)[4] = lbmodel.coeff;

	for (i=0;i<n_veloc;i++) {
	  local_n[i] = n_tmp[i] + coeff[i][1] * scalar(delta_j,c[i]);
	}

#endif

#ifdef ADDITIONAL_CHECKS
	lb_check_negative_n(index);
#endif

      }
    }
  }

}

/** Coupling of a particle to viscous fluid with Stokesian friction.
 * 
 * Section II.C. Ahlrichs and Duenweg, JCP 111(17):8225 (1999)
 *
 * @param p          The coupled particle (Input).
 * @param force      Coupling force between particle and fluid (Output).
 */
MDINLINE void lb_viscous_momentum_exchange(Particle *p, double force[3], double *flowfield) {

  int x,y,z;
  int node_index[8];
  double delta[6];
  double local_rho[1], local_j[3], interpolated_u[3], delta_j[3];
#ifdef ADDITIONAL_CHECKS
  double old_rho[8];
#endif

  ONEPART_TRACE(if(p->p.identity==check_id) fprintf(stderr,"%d: OPT: f = (%.3e,%.3e,%.3e)\n",this_node,p->f.f[0],p->f.f[1],p->f.f[2]));

  /* determine elementary lattice cell surrounding the particle 
     and the relative position of the particle in this cell */ 
  map_position_to_lattice(&lblattice,p->r.p,node_index,delta) ;

  ONEPART_TRACE(if(p->p.identity==check_id) fprintf(stderr,"%d: OPT: LB delta=(%.3f,%.3f,%.3f,%.3f,%.3f,%.3f) pos=(%.3f,%.3f,%.3f)\n",this_node,delta[0],delta[1],delta[2],delta[3],delta[4],delta[5],p->r.p[0],p->r.p[1],p->r.p[2]));

  /* calculate fluid velocity at particle's position
     this is done by linear interpolation
     (Eq. (11) Ahlrichs and Duenweg, JCP 111(17):8225 (1999)) */
  interpolated_u[0] = interpolated_u[1] = interpolated_u[2] = 0.0 ;
  for (z=0;z<2;z++) {
    for (y=0;y<2;y++) {
      for (x=0;x<2;x++) {
	
	*local_rho = flowfield[4*node_index[(z*2+y)*2+x]];
        local_j[0] = flowfield[4*node_index[(z*2+y)*2+x]+1];
        local_j[1] = flowfield[4*node_index[(z*2+y)*2+x]+2];
        local_j[2] = flowfield[4*node_index[(z*2+y)*2+x]+3];

#ifdef ADDITIONAL_CHECKS
	old_rho[(z*2+y)*2+x] = *local_rho;
#endif

	interpolated_u[0] += delta[3*x+0]*delta[3*y+1]*delta[3*z+2]*local_j[0]/(*local_rho);
	interpolated_u[1] += delta[3*x+0]*delta[3*y+1]*delta[3*z+2]*local_j[1]/(*local_rho);	  
	interpolated_u[2] += delta[3*x+0]*delta[3*y+1]*delta[3*z+2]*local_j[2]/(*local_rho) ;

      }
    }
  }
  
  ONEPART_TRACE(if(p->p.identity==check_id) fprintf(stderr,"%d: OPT: LB u = (%.16e,%.3e,%.3e) v = (%.16e,%.3e,%.3e)\n",this_node,interpolated_u[0],interpolated_u[1],interpolated_u[2],p->m.v[0],p->m.v[1],p->m.v[2]));

  /* calculate viscous force
   * take care to rescale velocities with time_step and transform to MD units 
   * (Eq. (9) Ahlrichs and Duenweg, JCP 111(17):8225 (1999)) */
  force[0] = - lbpar.friction * (p->m.v[0]/time_step - interpolated_u[0]*agrid/tau);
  force[1] = - lbpar.friction * (p->m.v[1]/time_step - interpolated_u[1]*agrid/tau);
  force[2] = - lbpar.friction * (p->m.v[2]/time_step - interpolated_u[2]*agrid/tau);

  ONEPART_TRACE(if(p->p.identity==check_id) fprintf(stderr,"%d: OPT: LB f_drag = (%.6e,%.3e,%.3e)\n",this_node,force[0],force[1],force[2]));

  ONEPART_TRACE(if(p->p.identity==check_id) fprintf(stderr,"%d: OPT: LB f_random = (%.6e,%.3e,%.3e)\n",this_node,p->lc.f_random[0],p->lc.f_random[1],p->lc.f_random[2]));

  force[0] = force[0] + p->lc.f_random[0];
  force[1] = force[1] + p->lc.f_random[1];
  force[2] = force[2] + p->lc.f_random[2];

  ONEPART_TRACE(if(p->p.identity==check_id) fprintf(stderr,"%d: OPT: LB f_tot = (%.6e,%.3e,%.3e)\n",this_node,force[0],force[1],force[2]));
      
  /* transform momentum transfer to lattice units
     (Eq. (12) Ahlrichs and Duenweg, JCP 111(17):8225 (1999)) */
  delta_j[0] = - force[0]*time_step*tau/agrid;
  delta_j[1] = - force[1]*time_step*tau/agrid;
  delta_j[2] = - force[2]*time_step*tau/agrid;
    
  lb_transfer_momentum(delta_j,node_index,delta);

#ifdef ADDITIONAL_CHECKS
  int i;
  for (i=0;i<8;i++) {
    lb_calc_local_rho(node_index[i],local_rho);
    if (fabs(*local_rho-old_rho[i]) > ROUND_ERROR_PREC) {
      char *errtxt = runtime_error(128);
      ERROR_SPRINTF(errtxt,"{108 Mass loss/gain %le in lb_viscous_momentum_exchange for particle %d} ",*local_rho-old_rho[i],p->p.identity);
    }
  }
#endif

}

/** Calculate particle lattice interactions.
 * So far, only viscous coupling with Stokesian friction is
 * implemented.
 * Include all particle-lattice forces in this function.
 * The function is called from \ref force_calc.
 *
 * Parallelizing the fluid particle coupling is not straightforward
 * because drawing of random numbers makes the whole thing nonlocal.
 * One way to do it is to treat every particle only on one node, i.e.
 * the random numbers need not be communicated. The particles that are 
 * not fully inside the local lattice are taken into account via their
 * ghost images on the neighbouring nodes. But this requires that the 
 * correct values of the surrounding lattice nodes are available on 
 * the respective node, which means that we have to communicate the 
 * halo regions before treating the ghost particles. Moreover, after 
 * determining the ghost couplings, we have to communicate back the 
 * halo region such that all local lattice nodes have the correct values.
 * Thus two communication phases are involved which will most likely be 
 * the bottleneck of the computation.
 *
 * Another way of dealing with the particle lattice coupling is to 
 * treat a particle and all of it's images explicitly. This requires the
 * communication of the random numbers used in the calculation of the 
 * coupling force. The problem is now that, if random numbers have to 
 * be redrawn, we cannot efficiently determine which particles and which 
 * images have to be re-calculated. We therefore go back to the outset
 * and go through the whole system again until no failure occurs during
 * such a sweep. In the worst case, this is very inefficient because
 * many things are recalculated although they actually don't need.
 * But we can assume that this happens extremely rarely and then we have
 * on average only one communication phase for the random numbers, which
 * probably makes this method preferable compared to the above one.
 */
void calc_particle_lattice_ia() {
 
  int i, c, np;
  Cell *cell ;
  Particle *p ;
  double force[3];
  double *flowfield;

  if (transfer_momentum) {

    flowfield = malloc(lblattice.halo_grid_volume*4*sizeof(double));

    /* exchange halo regions */
    halo_communication(&update_halo_comm,**lbfluid);
#ifdef ADDITIONAL_CHECKS
    //fprintf(stderr,"calc_particle_lattice_ia() checking halos\n");
    lb_check_halo_regions();
#endif

    for (i=0; i<lblattice.halo_grid_volume; i++) {
      lb_calc_local_fields(i, &flowfield[4*i], &flowfield[4*i+1], NULL, 0);
    }
    
    /* draw random numbers for local particles */
    for (c=0;c<local_cells.n;c++) {
      cell = local_cells.cell[c] ;
      p = cell->part ;
      np = cell->n ;
      for (i=0;i<np;i++) {
	p[i].lc.f_random[0] = -lb_coupl_pref*(d_random()-0.5);
	p[i].lc.f_random[1] = -lb_coupl_pref*(d_random()-0.5);
	p[i].lc.f_random[2] = -lb_coupl_pref*(d_random()-0.5);

#ifdef ADDITIONAL_CHECKS
	rancounter += 3;
#endif
      }
    }
    
    /* communicate the random numbers */
    ghost_communicator(&cell_structure.ghost_lbcoupling_comm) ;
    
    /* local cells */
    for (c=0;c<local_cells.n;c++) {
      cell = local_cells.cell[c] ;
      p = cell->part ;
      np = cell->n ;

      for (i=0;i<np;i++) {

	lb_viscous_momentum_exchange(&p[i],force,flowfield) ;

	/* add force to the particle */
	p[i].f.f[0] += force[0];
	p[i].f.f[1] += force[1];
	p[i].f.f[2] += force[2];

	ONEPART_TRACE(if(p->p.identity==check_id) fprintf(stderr,"%d: OPT: LB f = (%.6e,%.3e,%.3e)\n",this_node,p->f.f[0],p->f.f[1],p->f.f[2]));
  
      }

    }

    /* ghost cells */
    for (c=0;c<ghost_cells.n;c++) {
      cell = ghost_cells.cell[c] ;
      p = cell->part ;
      np = cell->n ;

      for (i=0;i<np;i++) {
	/* for ghost particles we have to check if they lie
	 * in the range of the local lattice nodes */
	if (p[i].r.p[0] >= my_left[0]-lblattice.agrid && p[i].r.p[0] < my_right[0]
	    && p[i].r.p[1] >= my_left[1]-lblattice.agrid && p[i].r.p[1] < my_right[1]
	    && p[i].r.p[2] >= my_left[2]-lblattice.agrid && p[i].r.p[2] < my_right[2]) {

	  ONEPART_TRACE(if(p[i].p.identity==check_id) fprintf(stderr,"%d: OPT: LB coupling of ghost particle:\n",this_node));

	  lb_viscous_momentum_exchange(&p[i],force,flowfield) ;

	  /* ghosts must not have the force added! */

	  ONEPART_TRACE(if(p->p.identity==check_id) fprintf(stderr,"%d: OPT: LB f = (%.6e,%.3e,%.3e)\n",this_node,p->f.f[0],p->f.f[1],p->f.f[2]));

	}
      }
    }

    free(flowfield);

  }

}

/***********************************************************************/

/** Calculate the average density of the fluid in the system.
 * This function has to be called after changing the density of
 * a local lattice site in order to set lbpar.rho consistently. */
void lb_calc_average_rho() {

  int x, y, z, index;
  double rho, local_rho, sum_rho;

  rho = 0.0;
  index = 0;
  for (z=1; z<=lblattice.grid[2]; z++) {
    for (y=1; y<=lblattice.grid[1]; y++) {
      for (x=1; x<=lblattice.grid[0]; x++) {
	
	lb_calc_local_rho(index, &rho);
	local_rho += rho;

	index++;
      }
      index += 2;
    }
    index += 2*lblattice.halo_grid[0];
  }

  MPI_Allreduce(&rho, &sum_rho, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);

  /* calculate average density in MD units */
  lbpar.rho = sum_rho / (box_l[0]*box_l[1]*box_l[2]);

}

/** Returns the hydrodynamic fields of a local lattice site.
 * @param index The index of the lattice site within the local domain (Input)
 * @param rho   Local density of the fluid (Output)
 * @param j     Local momentum of the fluid (Output)
 * @param pi    Local stress tensor of the fluid (Output)
 */
#if 0
void lb_get_local_fields(int index, double *rho, double *j, double *pi) {

  int i,k,m;

  lb_calc_local_fields(node,1);

  *rho = *local_rho;
  m = 0;
  for (i=0;i<3;i++) {
    j[i] = local_j[i];
    for (k=0;k<=i;k++) {
      pi[m] = local_pi[m];
      m++;
    }
  }

}
#endif

/** Sets the hydrodynamic fields on a local lattice site.
 * @param index The index of the lattice site within the local domain (Input)
 * @param rho   Local density of the fluid (Input)
 * @param v     Local velocity of the fluid (Input)
 */
#if 0
void lb_set_local_fields(int index, const double rho, const double *v, const double *pi) {

  double *local_rho = node->rho;
  double *local_j   = node->j;
  double *local_pi  = node->pi;

  *local_rho = rho;

  local_j[0] = rho * v[0];
  local_j[1] = rho * v[1];
  local_j[2] = rho * v[2];
  
  local_pi[0] = pi[0];
  local_pi[1] = pi[1];
  local_pi[2] = pi[2];
  local_pi[3] = pi[3];
  local_pi[4] = pi[4];
  local_pi[5] = pi[5];

  /* calculate populations according to equilibrium distribution */
  lb_calc_n_equilibrium(node);

}
#endif

/*@}*/

/***********************************************************************/
/** \name TCL stuff */
/***********************************************************************/

static int lb_parse_set_fields(Tcl_Interp *interp, int argc, char **argv, int *change, int *ind) {

  int k, index, node, grid[3];
  double rho, j[3], pi[6];

  *change = 4 ;
  if (argc < 4) return TCL_ERROR ;
  if (!ARG0_IS_D(rho)) return TCL_ERROR ;
  for (k=0;k<3;k++) {
    if (!ARG_IS_D(k+1,j[k])) return TCL_ERROR ;
  }
    
  node = map_lattice_to_node(&lblattice,ind,grid);
  index = get_linear_index(ind[0],ind[1],ind[2],lblattice.halo_grid);

  /* transform to lattice units */
  rho  *= agrid*agrid*agrid;
  j[0] *= tau/agrid;
  j[1] *= tau/agrid;
  j[2] *= tau/agrid;

  pi[0] = rho*lbmodel.c_sound_sq + j[0]*j[0]/rho;
  pi[2] = rho*lbmodel.c_sound_sq + j[1]*j[1]/rho;
  pi[5] = rho*lbmodel.c_sound_sq + j[2]*j[2]/rho;
  pi[1] = j[0]*j[1]/rho;
  pi[3] = j[0]*j[2]/rho;
  pi[4] = j[1]*j[2]/rho;

  mpi_send_fluid(node,index,rho,j,pi) ;

  lb_calc_average_rho();
  lb_reinit_parameters();

  return TCL_OK ;

}

static int lb_print_local_fields(Tcl_Interp *interp, int argc, char **argv, int *change, int *ind) {

  char buffer[256+4*TCL_DOUBLE_SPACE+3*TCL_INTEGER_SPACE];
  int index, node, grid[3];
  double rho, j[3], pi[6];

  *change = 0;

  sprintf(buffer, "%d", ind[0]) ;
  Tcl_AppendResult(interp, buffer, (char *)NULL);
  sprintf(buffer, "%d", ind[1]) ;
  Tcl_AppendResult(interp, buffer, (char *)NULL);
  sprintf(buffer, "%d", ind[2]) ;
  Tcl_AppendResult(interp, buffer, (char *)NULL);

  node = map_lattice_to_node(&lblattice,ind,grid);
  index = get_linear_index(ind[0],ind[1],ind[2],lblattice.halo_grid);
  
  mpi_recv_fluid(node,index,&rho,j,pi) ;

  /* transform to MD units */
  rho  *= 1./(agrid*agrid*agrid);
  j[0] *= agrid/tau;
  j[1] *= agrid/tau;
  j[2] *= agrid/tau;

  Tcl_PrintDouble(interp, rho, buffer);
  Tcl_AppendResult(interp, buffer, (char *)NULL);
  Tcl_PrintDouble(interp, j[0], buffer);
  Tcl_AppendResult(interp, buffer, (char *)NULL);
  Tcl_PrintDouble(interp, j[1], buffer);
  Tcl_AppendResult(interp, buffer, (char *)NULL);
  Tcl_PrintDouble(interp, j[2], buffer);
  Tcl_AppendResult(interp, buffer, (char *)NULL);
    
  return TCL_OK ;

}

MDINLINE void lbnode_print_rho(Tcl_Interp *interp, double rho) {
  char buffer[TCL_DOUBLE_SPACE];

  Tcl_PrintDouble(interp, rho, buffer);
  Tcl_AppendResult(interp, buffer, " ", (char *)NULL);

}

MDINLINE void lbnode_print_v(Tcl_Interp *interp, double *j, double rho) {
  char buffer[TCL_DOUBLE_SPACE];
  
  Tcl_PrintDouble(interp, j[0]/rho, buffer);
  Tcl_AppendResult(interp, buffer, " ", (char *)NULL);
  Tcl_PrintDouble(interp, j[1]/rho, buffer);
  Tcl_AppendResult(interp, buffer, " ", (char *)NULL);
  Tcl_PrintDouble(interp, j[2]/rho, buffer);
  Tcl_AppendResult(interp, buffer, " ", (char *)NULL); 

}

MDINLINE void lbnode_print_pi(Tcl_Interp *interp, double *pi) {
  char buffer[TCL_DOUBLE_SPACE];

  Tcl_PrintDouble(interp, pi[0], buffer);
  Tcl_AppendResult(interp, buffer, " ", (char *)NULL);
  Tcl_PrintDouble(interp, pi[1], buffer);
  Tcl_AppendResult(interp, buffer, " ", (char *)NULL);
  Tcl_PrintDouble(interp, pi[2], buffer);
  Tcl_AppendResult(interp, buffer, " ", (char *)NULL);
  Tcl_PrintDouble(interp, pi[3], buffer);
  Tcl_AppendResult(interp, buffer, " ", (char *)NULL);
  Tcl_PrintDouble(interp, pi[4], buffer);
  Tcl_AppendResult(interp, buffer, " ", (char *)NULL);
  Tcl_PrintDouble(interp, pi[5], buffer);
  Tcl_AppendResult(interp, buffer, " ", (char *)NULL);
      
}

MDINLINE void lbnode_print_pi_neq(Tcl_Interp *interp, double rho, double *j, double *pi) {
  char buffer[TCL_DOUBLE_SPACE];
  double pi_neq[6];

  pi_neq[0] = pi[0] - rho*lbmodel.c_sound_sq - j[0]*j[0]/rho;
  pi_neq[2] = pi[2] - rho*lbmodel.c_sound_sq - j[1]*j[1]/rho;
  pi_neq[5] = pi[5] - rho*lbmodel.c_sound_sq - j[2]*j[2]/rho;
  pi_neq[1] = pi[1] - j[0]*j[1]/rho;
  pi_neq[3] = pi[3] - j[0]*j[2]/rho;
  pi_neq[4] = pi[4] - j[1]*j[2]/rho;

  Tcl_PrintDouble(interp, pi_neq[0], buffer);
  Tcl_AppendResult(interp, buffer, " ", (char *)NULL);
  Tcl_PrintDouble(interp, pi_neq[1], buffer);
  Tcl_AppendResult(interp, buffer, " ", (char *)NULL);
  Tcl_PrintDouble(interp, pi_neq[2], buffer);
  Tcl_AppendResult(interp, buffer, " ", (char *)NULL);
  Tcl_PrintDouble(interp, pi_neq[3], buffer);
  Tcl_AppendResult(interp, buffer, " ", (char *)NULL);
  Tcl_PrintDouble(interp, pi_neq[4], buffer);
  Tcl_AppendResult(interp, buffer, " ", (char *)NULL);
  Tcl_PrintDouble(interp, pi_neq[5], buffer);
  Tcl_AppendResult(interp, buffer, " ", (char *)NULL);

}

static int lbnode_parse_print(Tcl_Interp *interp, int argc, char **argv, int *ind) {
  int node, index, grid[3];
  double rho, j[3], pi[6];

  node = map_lattice_to_node(&lblattice,ind,grid);
  index = get_linear_index(ind[0],ind[1],ind[2],lblattice.halo_grid);
  
  mpi_recv_fluid(node,index,&rho,j,pi);

  while (argc > 0) {
    if (ARG0_IS_S("rho") || ARG0_IS_S("density")) 
      lbnode_print_rho(interp, rho);
    else if (ARG0_IS_S("u") || ARG0_IS_S("v") || ARG0_IS_S("velocity"))
      lbnode_print_v(interp, j, rho);
    else if (ARG0_IS_S("pi") || ARG0_IS_S("pressure"))
      lbnode_print_pi(interp, pi);
    else if (ARG0_IS_S("pi_neq")) /* this has to come after pi */
      lbnode_print_pi_neq(interp, rho, j, pi);
    else {
      Tcl_ResetResult(interp);
      Tcl_AppendResult(interp, "unknown fluid data \"", argv[0], "\" requested", (char *)NULL);
      return TCL_ERROR;
    }
    --argc; ++argv;
  }

  return TCL_OK;
}

static int lbfluid_parse_tau(Tcl_Interp *interp, int argc, char *argv[], int *change) {
    double tau;

    if (argc < 1) {
	Tcl_AppendResult(interp, "tau requires 1 argument", NULL);
	return TCL_ERROR;
    }
    if (!ARG0_IS_D(tau)) {
	Tcl_AppendResult(interp, "wrong  argument for tau", (char *)NULL);
	return TCL_ERROR;
    }
    if (tau < 0.0) {
	Tcl_AppendResult(interp, "tau must be positive", (char *)NULL);
	return TCL_ERROR;
    }
    else if ((time_step >= 0.0) && (tau < time_step)) {
      Tcl_AppendResult(interp, "tau must be larger than MD time_step", (char *)NULL);
      return TCL_ERROR;
    }

    *change = 1;
    lbpar.tau = tau;

    mpi_bcast_lb_params(LBPAR_TAU);

    return TCL_OK;
}

static int lbfluid_parse_agrid(Tcl_Interp *interp, int argc, char *argv[], int *change) {

    if (argc < 1) {
	Tcl_AppendResult(interp, "agrid requires 1 argument", (char *)NULL);
	return TCL_ERROR;
    }
    if (!ARG0_IS_D(agrid)) {
	Tcl_AppendResult(interp, "wrong argument for agrid", (char *)NULL);
	return TCL_ERROR;
    }
    if (agrid <= 0.0) {
	Tcl_AppendResult(interp, "agrid must be positive", (char *)NULL);
	return TCL_ERROR;
    }

    *change = 1;
    lbpar.agrid = agrid;

    mpi_bcast_lb_params(LBPAR_AGRID);
 
    return TCL_OK;
}

static int lbfluid_parse_density(Tcl_Interp *interp, int argc, char *argv[], int *change) {
    double density;

    if (argc < 1) {
	Tcl_AppendResult(interp, "density requires 1 argument", (char *)NULL);
	return TCL_ERROR;
    }
    if (!ARG0_IS_D(density)) {
	Tcl_AppendResult(interp, "wrong argument for density", (char *)NULL);
	return TCL_ERROR;
    }
    if (density <= 0.0) {
	Tcl_AppendResult(interp, "density must be positive", (char *)NULL);
	return TCL_ERROR;
    }

    *change = 1;
    lbpar.rho = density;

    mpi_bcast_lb_params(LBPAR_DENSITY);
 
    return TCL_OK;
}

static int lbfluid_parse_viscosity(Tcl_Interp *interp, int argc, char *argv[], int *change) {
    double viscosity;

    if (argc < 1) {
	Tcl_AppendResult(interp, "viscosity requires 1 argument", (char *)NULL);
	return TCL_ERROR;
    }
    if (!ARG0_IS_D(viscosity)) {
	Tcl_AppendResult(interp, "wrong argument for viscosity", (char *)NULL);
	return TCL_ERROR;
    }
    if (viscosity <= 0.0) {
	Tcl_AppendResult(interp, "viscosity must be positive", (char *)NULL);
	return TCL_ERROR;
    }

    *change = 1;
    lbpar.viscosity = viscosity;

    mpi_bcast_lb_params(LBPAR_VISCOSITY);
 
    return TCL_OK;
}

static int lbfluid_parse_bulk_visc(Tcl_Interp *interp, int argc, char *argv[], int *change) {
  double bulk_visc;

  if (argc < 1) {
    Tcl_AppendResult(interp, "bulk_viscosity requires 1 argument", (char *)NULL);
    return TCL_ERROR;
  }
  if (!ARG0_IS_D(bulk_visc)) {
    Tcl_AppendResult(interp, "wrong argument for bulk_viscosity", (char *)NULL);
    return TCL_ERROR;
  }
  if (bulk_visc < 0.0) {
    Tcl_AppendResult(interp, "bulk_viscosity must be positive", (char *)NULL);
    return TCL_ERROR;
  }

  *change =1;
  lbpar.bulk_viscosity = bulk_visc;

  mpi_bcast_lb_params(LBPAR_BULKVISC);

  return TCL_OK;

}

static int lbfluid_parse_friction(Tcl_Interp *interp, int argc, char *argv[], int *change) {
    double friction;

    if (argc < 1) {
	Tcl_AppendResult(interp, "friction requires 1 argument", (char *)NULL);
	return TCL_ERROR;
    }
    if (!ARG0_IS_D(friction)) {
	Tcl_AppendResult(interp, "wrong argument for friction", (char *)NULL);
	return TCL_ERROR;
    }
    if (friction <= 0.0) {
	Tcl_AppendResult(interp, "friction must be positive", (char *)NULL);
	return TCL_ERROR;
    }

    *change = 1;
    lbpar.friction = friction;

    mpi_bcast_lb_params(LBPAR_FRICTION);
 
    return TCL_OK;
}

static int lbfluid_parse_ext_force(Tcl_Interp *interp, int argc, char *argv[], int *change) {
#ifdef EXTERNAL_FORCES
    double ext_f[3];
    if (argc < 3) {
	Tcl_AppendResult(interp, "ext_force requires 3 arguments", (char *)NULL);
	return TCL_ERROR;
    }
    else {
 	if (!ARG_IS_D(0, ext_f[0])) return TCL_ERROR;
	if (!ARG_IS_D(1, ext_f[1])) return TCL_ERROR;
	if (!ARG_IS_D(2, ext_f[2])) return TCL_ERROR;
    }
    
    *change = 3;

    /* external force is stored in lattice units */
    lbpar.ext_force[0] = ext_f[0]*agrid*agrid*tau*tau;
    lbpar.ext_force[1] = ext_f[1]*agrid*agrid*tau*tau;
    lbpar.ext_force[2] = ext_f[2]*agrid*agrid*tau*tau;
    
    mpi_bcast_lb_params(LBPAR_EXTFORCE);
 
    return TCL_OK;
#else
  Tcl_AppendResult(interp, "EXTERNAL_FORCES not compiled in!", NULL);
  return TCL_ERROR;
#endif
}
#endif /* LB */

/** Parser for the \ref lbnode command. */
int lbnode_cmd(ClientData data, Tcl_Interp *interp, int argc, char **argv) {
#ifdef LB
   int err=TCL_ERROR;
   int coord[3];

   --argc; ++argv;
   
   if (argc < 3) {
     Tcl_AppendResult(interp, "too few arguments for lbnode", (char *)NULL);
     return TCL_ERROR;
   }

   if (!ARG_IS_I(0,coord[0]) || !ARG_IS_I(1,coord[1]) || !ARG_IS_I(2,coord[2])) {
     Tcl_AppendResult(interp, "wrong arguments for lbnode", (char *)NULL);
     return TCL_ERROR;
   } else {
     argc-=3; argv+=3;

     if (ARG0_IS_S("print"))
       err = lbnode_parse_print(interp, argc-1, argv+1, coord);
     else {
       Tcl_AppendResult(interp, "unknown feature \"", argv[0], "\" of lbnode", (char *)NULL);
       err = TCL_ERROR;
     }
     
   }
     
   return err;
#else /* !defined LB */
  Tcl_AppendResult(interp, "LB is not compiled in!", NULL);
  return TCL_ERROR;
#endif
}

/** Parser for the \ref lbfluid command. */
int lbfluid_cmd(ClientData data, Tcl_Interp *interp, int argc, char **argv) {
#ifdef LB
  int err = TCL_OK;
  int change = 0;
  
  argc--; argv++;

  if (argc < 1) {
      Tcl_AppendResult(interp, "too few arguments to \"lbfluid\"", (char *)NULL);
      err = TCL_ERROR;
  }
  else if (ARG0_IS_S("off")) {
    err = TCL_ERROR;
  }
  else if (ARG0_IS_S("init")) {
    err = TCL_ERROR;
  }
  else while (argc > 0) {
      if (ARG0_IS_S("grid") || ARG0_IS_S("agrid"))
	  err = lbfluid_parse_agrid(interp, argc-1, argv+1, &change);
      else if (ARG0_IS_S("tau"))
	  err = lbfluid_parse_tau(interp, argc-1, argv+1, &change);
      else if (ARG0_IS_S("density"))
	  err = lbfluid_parse_density(interp, argc-1, argv+1, &change);
      else if (ARG0_IS_S("viscosity"))
	  err = lbfluid_parse_viscosity(interp, argc-1, argv+1, &change);
      else if (ARG0_IS_S("bulk_viscosity"))
	  err = lbfluid_parse_bulk_visc(interp, argc-1, argv+1, &change);
      else if (ARG0_IS_S("friction") || ARG0_IS_S("coupling"))
	  err = lbfluid_parse_friction(interp, argc-1, argv+1, &change);
      else if (ARG0_IS_S("ext_force"))
	  err = lbfluid_parse_ext_force(interp, argc-1, argv+1, &change);
      else {
	  Tcl_AppendResult(interp, "unknown feature \"", argv[0],"\" of lbfluid", (char *)NULL);
	  err = TCL_ERROR ;
      }

      if ((err = mpi_gather_runtime_errors(interp, err))) break;

      argc -= (change + 1);
      argv += (change + 1);
  }

  lattice_switch = (lattice_switch | LATTICE_LB) ;
  mpi_bcast_parameter(FIELD_LATTICE_SWITCH) ;

  /* thermo_switch is retained for backwards compatibility */
  thermo_switch = (thermo_switch | THERMO_LB);
  mpi_bcast_parameter(FIELD_THERMO_SWITCH);

  return err;    
#else /* !defined LB */
  Tcl_AppendResult(interp, "LB is not compiled in!", NULL);
  return TCL_ERROR;
#endif
}

/*@}*/
