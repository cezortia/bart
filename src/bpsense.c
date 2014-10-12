/* Copyright 2014. The Regents of the University of California.
 * All rights reserved. Use of this source code is governed by 
 * a BSD-style license which can be found in the LICENSE file.
 *
 * Authors: 
 * 2014 Jonathan Tamir <jtamir@eecs.berkeley.edu>
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <assert.h>
#include <stdbool.h>
#include <complex.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>

#include "num/multind.h"
#include "num/flpmath.h"
#include "num/fft.h"
#include "num/init.h"
#include "num/ops.h"
#include "num/iovec.h"

#include "linops/someops.h"
#include "linops/linop.h"
#include "linops/tv.h"

#include "iter/thresh.h"
#include "iter/iter.h"

#include "sense/bprecon.h"
#include "sense/optcom.h"

#include "wavelet2/wavelet.h"

#include "misc/debug.h"
#include "misc/mri.h"
#include "misc/mmio.h"
#include "misc/misc.h"



static void usage(const char* name, FILE* fd)
{
	fprintf(fd, "Usage: %s [-g] [-r l2lambda] [-c] [-e eps] [-u rho] <kspace> <sensitivities> <output>\n", name);
}

static void help(void)
{
	printf( "\n"
		"Perform basis pursuit denoising for SENSE/ESPIRiT reconstruction:\n"
		"min_x ||T x||_1 + lambda/2 ||x||_2^2 subject to: ||y - Ax||_2 <= eps\n"
		"\n"
		"-e eps\tdata consistency error\n"
		"-r lambda\tl2 regularization parameter\n"
		"-u rho\tADMM penalty parameter\n"
		"-c\treal-value constraint\n"
		"-t\tuse TV norm\n"
		"-F\ttruth image\n");
}


int main(int argc, char* argv[])
{
	// -----------------------------------------------------------
	// set up conf and option parser
	
	struct bpsense_conf conf;
	struct iter_admm_conf iconf;
	memcpy(&conf, &bpsense_defaults, sizeof(struct bpsense_conf));
	memcpy(&iconf, &iter_admm_defaults, sizeof(struct iter_admm_conf));
	conf.iconf = &iconf;
	conf.iconf->rho = 10; // more sensibile default

	bool usegpu = false;
	const char* psf = NULL;
	char image_truth_fname[100];
	_Bool im_truth = false;
	_Bool use_tvnorm = false;

	double start_time = timestamp();

	int c;
	while (-1 != (c = getopt(argc, argv, "F:r:e:i:u:p:tcgh"))) {
		switch(c) {
		case 'F':
			im_truth = true;
			sprintf(image_truth_fname, "%s", optarg);
			break;

		case 'r':
			conf.lambda = atof(optarg);
			break;

		case 'e':
			conf.eps = atof(optarg);
			break;

		case 'i':
			conf.iconf->maxiter = atoi(optarg);
			break;

		case 'h':
			usage(argv[0], stdout);
			help();
			exit(0);

		case 'c':
			conf.rvc = true;
			break;

		case 'u':
			conf.iconf->rho = atof(optarg);
			break;

		case 'g':
			usegpu = true;
			break;

		case 'p':
			psf = strdup(optarg);
			break;

		case 't':
			use_tvnorm = true;
			break;

		default:
			usage(argv[0], stderr);
			exit(1);
		}
	}
	if (argc - optind != 3) {

		usage(argv[0], stderr);
		exit(1);
	}

	// -----------------------------------------------------------
	// load data and print some info about the recon

	int N = DIMS;

	long dims[N];
	long dims1[N];
	long img_dims[N];
	long ksp_dims[N];

	complex float* kspace_data = load_cfl(argv[optind + 0], N, ksp_dims);
	complex float* sens_maps = load_cfl(argv[optind + 1], N, dims);

	for (int i = 0; i < 4; i++) {	// sizes2[4] may be > 1
		if (ksp_dims[i] != dims[i]) {
		
			fprintf(stderr, "Dimensions of kspace and sensitivities do not match!\n");
			exit(1);
		}
	}

	assert(1 == ksp_dims[MAPS_DIM]);


	(usegpu ? num_init_gpu : num_init)();

	if (dims[MAPS_DIM] > 1) 
		debug_printf(DP_INFO, "%ld maps.\nESPIRiT reconstruction.\n", dims[4]);

	if (conf.lambda > 0.)
		debug_printf(DP_INFO, "l2 regularization: %f\n", conf.lambda);

	if (use_tvnorm)
		debug_printf(DP_INFO, "use Total Variation\n");
	else
		debug_printf(DP_INFO, "use Wavelets\n");

	if (im_truth)
		debug_printf(DP_INFO, "Compare to truth\n");

	md_select_dims(N, ~(COIL_FLAG | MAPS_FLAG), dims1, dims);
	md_select_dims(N, ~COIL_FLAG, img_dims, dims);


	// -----------------------------------------------------------
	// initialize sampling pattern

	complex float* pattern = NULL;
	long pat_dims[N];

	if (NULL != psf) {

		pattern = load_cfl(psf, N, pat_dims);

		// FIXME: check compatibility
	} else {

		pattern = md_alloc(N, dims1, CFL_SIZE);
		estimate_pattern(N, ksp_dims, COIL_DIM, pattern, kspace_data);
	}

	
	// -----------------------------------------------------------
	// print some statistics

	size_t T = md_calc_size(N, dims1);
	long samples = (long)pow(md_znorm(N, dims1, pattern), 2.);
	debug_printf(DP_INFO, "Size: %ld Samples: %ld Acc: %.2f\n", T, samples, (float)T/(float)samples); 


	// -----------------------------------------------------------
	// fftmod to un-center data
	
	fftmod(N, ksp_dims, FFT_FLAGS, kspace_data, kspace_data);
	fftmod(N, dims, FFT_FLAGS, sens_maps, sens_maps);


	// -----------------------------------------------------------
	// apply scaling

	float scaling = estimate_scaling(ksp_dims, NULL, kspace_data);

	debug_printf(DP_INFO, "Scaling: %f\n", scaling);

	if (scaling != 0.)
		md_zsmul(N, ksp_dims, kspace_data, kspace_data, 1. / scaling);


	// -----------------------------------------------------------
	// create l1 prox operator and transform

	long minsize[DIMS] = { [0 ... DIMS - 1] = 1 };
	minsize[0] = MIN(img_dims[0], 16);
	minsize[1] = MIN(img_dims[1], 16);
	minsize[2] = MIN(img_dims[2], 16);

	const struct linop_s* l1op = NULL;
	const struct operator_p_s* l1prox = NULL;

	if (use_tvnorm) {
		l1op = tv_init(DIMS, img_dims, FFT_FLAGS);
		l1prox = prox_thresh_create(DIMS + 1, linop_codomain(l1op)->dims, 1., 0u, usegpu);
		conf.l1op_obj = l1op;
	}
	else {
		bool randshift = true;
		l1op = linop_identity_create(DIMS, img_dims);
		conf.l1op_obj = wavelet_create(DIMS, img_dims, FFT_FLAGS, minsize, false, usegpu);
		l1prox = prox_wavethresh_create(DIMS, img_dims, FFT_FLAGS, minsize, 1., randshift, usegpu);
	}


	// -----------------------------------------------------------
	// create image and load truth image
	
	complex float* image = create_cfl(argv[optind + 2], N, img_dims);
	
	md_clear(N, img_dims, image, CFL_SIZE);

	long img_truth_dims[DIMS];
	complex float* image_truth = NULL;

	if (im_truth)
		image_truth = load_cfl(image_truth_fname, DIMS, img_truth_dims);

	// -----------------------------------------------------------
	// call recon
	
	if (usegpu) 
#ifdef USE_CUDA
		bpsense_recon_gpu(&conf, dims, image, sens_maps, dims1, pattern, l1op, l1prox, ksp_dims, kspace_data, image_truth);
#else
		assert(0);
#endif
	else
		bpsense_recon(&conf, dims, image, sens_maps, dims1, pattern, l1op, l1prox, ksp_dims, kspace_data, image_truth);

	// -----------------------------------------------------------
	// cleanup

	if (NULL != psf)
		unmap_cfl(N, pat_dims, pattern);
	else
		md_free(pattern);

	operator_p_free(l1prox);
	linop_free(l1op);

	if (!use_tvnorm)
		linop_free(conf.l1op_obj);

	unmap_cfl(N, dims, sens_maps);
	unmap_cfl(N, ksp_dims, kspace_data);
	unmap_cfl(N, img_dims, image);

	double end_time = timestamp();
	debug_printf(DP_INFO, "Total Time: %f\n", end_time - start_time);

	exit(0);
}

