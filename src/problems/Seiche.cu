/*  Copyright 2011-2013 Alexis Herault, Giuseppe Bilotta, Robert A. Dalrymple, Eugenio Rustico, Ciro Del Negro

    Istituto Nazionale di Geofisica e Vulcanologia
        Sezione di Catania, Catania, Italy

    Università di Catania, Catania, Italy

    Johns Hopkins University, Baltimore, MD

    This file is part of GPUSPH.

    GPUSPH is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    GPUSPH is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with GPUSPH.  If not, see <http://www.gnu.org/licenses/>.
*/


#include <cmath>
#include <iostream>
#include <stdexcept>

#include "cudasimframework.cu"

#include "Seiche.h"
#include "particledefine.h"
#include "GlobalData.h"

Seiche::Seiche(GlobalData *_gdata) : Problem(_gdata)
{
	SETUP_FRAMEWORK(
		viscosity<SPSVISC>
	);

	addFilter(MLS_FILTER, 20);

	set_deltap(0.015f);
	H = .5f;
	l = sqrt(2)*H; w = l/2; h = 1.5*H;
	std::cout << "length= " << l<<"\n";
	std::cout << "width= " << w <<"\n";
	std::cout << "h = " << h <<"\n";

	// Size and origin of the simulation domain
	m_size = make_double3(l, w ,h);
	m_origin = make_double3(0.0, 0.0, 0.0);

	// SPH parameters
	m_simparams->dt = 0.00004f;
	m_simparams->dtadaptfactor = 0.2;
	m_simparams->buildneibsfreq = 10;
	m_simparams->tend=10.0f;
	m_simparams->gcallback=true;

	// Physical parameters
	m_physparams->gravity = make_float3(0.0, 0.0, -9.81f); //must be set first
	float g = length(m_physparams->gravity);
	add_fluid(1000.0);
	set_equation_of_state(0,  7.0f, 20.f);

    //set p1coeff,p2coeff, epsxsph here if different from 12.,6., 0.5
	m_physparams->dcoeff = 5.0f*g*H;
	m_physparams->r0 = m_deltap;

	// BC when using MK boundary condition: Coupled with m_simsparams->boundarytype=MK_BOUNDARY
	#define MK_par 2
	m_physparams->MK_K = g*H;
	m_physparams->MK_d = 1.1*m_deltap/MK_par;
	m_physparams->MK_beta = MK_par;
	#undef MK_par

	set_kinematic_visc(0, 5.0e-6f);
	m_physparams->artvisccoeff = 0.3f;
	m_physparams->smagfactor = 0.12*0.12*m_deltap*m_deltap;
	m_physparams->kspsfactor = (2.0/3.0)*0.0066*m_deltap*m_deltap;
	m_physparams->epsartvisc = 0.01*m_simparams->slength*m_simparams->slength;

	// Variable gravity terms:  starting with m_physparams->gravity as defined above
	m_gtstart=0.3;
	m_gtend=3.0;

	// Drawing and saving times
	add_writer(VTKWRITER, 0.1);

	// Name of problem used for directory creation
	m_name = "Seiche";
}


Seiche::~Seiche(void)
{
	release_memory();
}


void Seiche::release_memory(void)
{
	parts.clear();
	boundary_parts.clear();
}

float3 Seiche::g_callback(const double t)
{
	if(t > m_gtstart && t < m_gtend)
		m_physparams->gravity=make_float3(2.*sin(9.8*(t-m_gtstart)), 0.0, -9.81f);
	else
		m_physparams->gravity=make_float3(0.,0.,-9.81f);
	return m_physparams->gravity;
}


int Seiche::fill_parts()
{
	// distance between fluid box and wall
	float wd = m_deltap; //Used to be divided by 2


	parts.reserve(14000);

	experiment_box = Cube(Point(0, 0, 0), l, w, h);
	Cube fluid = Cube(Point(wd, wd, wd), l-2*wd, w-2*wd, H-2*wd);
	fluid.SetPartMass(m_deltap, m_physparams->rho0[0]);
	// InnerFill puts particle in the center of boxes of step m_deltap, hence at
	// m_deltap/2 from the sides, so the total distance between particles and walls
	// is m_deltap = r0
//	fluid.InnerFill(parts, m_deltap);
	fluid.Fill(parts,m_deltap,true);// it used to be InnerFill


	return parts.size() + boundary_parts.size();
}

uint Seiche::fill_planes()
{
	return 5;
}

void Seiche::copy_planes(double4 *planes)
{
	planes[0] = make_double4(0, 0, 1.0, 0);
	planes[1] = make_double4(0, 1.0, 0, 0);
	planes[2] = make_double4(0, -1.0, 0, w);
	planes[3] = make_double4(1.0, 0, 0, 0);
	planes[4] = make_double4(-1.0, 0, 0, l);
}


void Seiche::copy_to_array(BufferList &buffers)
{
	float4 *pos = buffers.getData<BUFFER_POS>();
	hashKey *hash = buffers.getData<BUFFER_HASH>();
	float4 *vel = buffers.getData<BUFFER_VEL>();
	particleinfo *info = buffers.getData<BUFFER_INFO>();

	std::cout << "Boundary parts: " << boundary_parts.size() << "\n";
	for (uint i = 0; i < boundary_parts.size(); i++) {
		vel[i] = make_float4(0, 0, 0, m_physparams->rho0[0]);
		info[i] = make_particleinfo(PT_BOUNDARY, 0, i);
		calc_localpos_and_hash(boundary_parts[i], info[i], pos[i], hash[i]);
	}
	int j = boundary_parts.size();
	std::cout << "Boundary part mass: " << pos[j-1].w << "\n";

	std::cout << "Fluid parts: " << parts.size() << "\n";
	for (uint i = j; i < j + parts.size(); i++) {
	//	vel[i] = make_float4(0, 0, 0, rho);
		vel[i] = make_float4(0, 0, 0, m_physparams->rho0[0]);
		info[i] = make_particleinfo(PT_FLUID, 0, i);
		calc_localpos_and_hash(parts[i-j], info[i], pos[i], hash[i]);
	//	float rho = density(H - pos[i].z,0);
	}
	j += parts.size();
	std::cout << "Fluid part mass: " << pos[j-1].w << "\n";
}
