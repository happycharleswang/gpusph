/*
 * GPUWorker.cpp
 *
 *  Created on: Dec 19, 2012
 *      Author: rustico
 */

#include "GPUWorker.h"
#include "buildneibs.cuh"
#include "forces.cuh"
#include "euler.cuh"

#include "cudabuffer.h"

// ostringstream
#include <sstream>
// FLT_MAX
#include "float.h"

GPUWorker::GPUWorker(GlobalData* _gdata, unsigned int _deviceIndex) {
	gdata = _gdata;
	m_deviceIndex = _deviceIndex;
	m_cudaDeviceNumber = gdata->device[m_deviceIndex];

	m_globalDeviceIdx = GlobalData::GLOBAL_DEVICE_ID(gdata->mpi_rank, _deviceIndex);

	// we know that GPUWorker is initialized when Problem was already
	m_simparams = gdata->problem->get_simparams();
	m_physparams = gdata->problem->get_physparams();

	// we also know Problem::fillparts() has already been called
	m_numInternalParticles = m_numParticles = gdata->s_hPartsPerDevice[m_deviceIndex];

	m_particleRangeBegin = 0;
	m_particleRangeEnd = m_numInternalParticles;

	m_numAllocatedParticles = 0;
	m_nGridCells = gdata->nGridCells;

	m_hostMemory = m_deviceMemory = 0;

	m_dBuffers << new CUDABuffer<BUFFER_POS>();
	m_dBuffers << new CUDABuffer<BUFFER_VEL>();
	m_dBuffers << new CUDABuffer<BUFFER_INFO>();

	m_dBuffers << new CUDABuffer<BUFFER_HASH>();
	m_dBuffers << new CUDABuffer<BUFFER_PARTINDEX>();
	m_dBuffers << new CUDABuffer<BUFFER_NEIBSLIST>(-1); // neib list is initialized to all bits set

	if (m_simparams->boundarytype == MF_BOUNDARY)
		m_dBuffers << new CUDABuffer<BUFFER_INVINDEX>();

	m_dBuffers << new CUDABuffer<BUFFER_FORCES>();
	if (m_simparams->xsph)
		m_dBuffers << new CUDABuffer<BUFFER_XSPH>();

	if (m_simparams->visctype == SPSVISC)
		m_dBuffers << new CUDABuffer<BUFFER_TAU>();

	if (m_simparams->savenormals)
		m_dBuffers << new CUDABuffer<BUFFER_NORMALS>();
	if (m_simparams->vorticity)
		m_dBuffers << new CUDABuffer<BUFFER_VORTICITY>();

	if (m_simparams->dtadapt) {
		m_dBuffers << new CUDABuffer<BUFFER_CFL>();
		m_dBuffers << new CUDABuffer<BUFFER_CFL_TEMP>();
		if (m_simparams->boundarytype == MF_BOUNDARY)
			m_dBuffers << new CUDABuffer<BUFFER_CFL_GAMMA>();
		if (m_simparams->visctype == KEPSVISC)
			m_dBuffers << new CUDABuffer<BUFFER_CFL_KEPS>();
	}

	if (m_simparams->boundarytype == MF_BOUNDARY) {
		m_dBuffers << new CUDABuffer<BUFFER_GRADGAMMA>();
		m_dBuffers << new CUDABuffer<BUFFER_BOUNDELEMENTS>();
		m_dBuffers << new CUDABuffer<BUFFER_VERTICES>();
		m_dBuffers << new CUDABuffer<BUFFER_PRESSURE>();
	}

	if (m_simparams->visctype == KEPSVISC) {
		m_dBuffers << new CUDABuffer<BUFFER_TKE>();
		m_dBuffers << new CUDABuffer<BUFFER_EPSILON>();
		m_dBuffers << new CUDABuffer<BUFFER_TURBVISC>();
		m_dBuffers << new CUDABuffer<BUFFER_STRAIN_RATE>();
		m_dBuffers << new CUDABuffer<BUFFER_DKDE>();
	}
}

GPUWorker::~GPUWorker() {
	// Free everything and pthread terminate
	// should check whether the pthread is still running and force its termination?
}

// Return the number of particles currently being handled (internal and r.o.)
uint GPUWorker::getNumParticles()
{
	return m_numParticles;
}

uint GPUWorker::getNumInternalParticles() {
	return m_numInternalParticles;
}

// Return the maximum number of particles the worker can handled (allocated)
uint GPUWorker::getMaxParticles()
{
	return m_numAllocatedParticles;
}

// Compute the bytes required for each particle.
// NOTE: this should be updated for each new device array!
// TODO FIXME MERGE not updated for keps e mf stuff
size_t GPUWorker::computeMemoryPerParticle()
{
	size_t tot = 0;

	BufferList::iterator buf = m_dBuffers.begin();
	const BufferList::iterator stop = m_dBuffers.end();
	while (buf != stop) {
		size_t contrib = buf->second->get_element_size()*buf->second->get_array_count();
		if (buf->first & BUFFER_NEIBSLIST)
			contrib *= m_simparams->maxneibsnum;
		// TODO compute a sensible estimate for the CFL contribution,
		// which is currently heavily overestimated
		else if (buf->first & BUFFERS_CFL)
			contrib /= 4;
		// particle index occupancy is double to account for memory allocated
		// by thrust::sort TODO refine
		else if (buf->first & BUFFER_PARTINDEX)
			contrib *= 2;

		tot += contrib;
#if _DEBUG_
		printf("with %s: %zu\n", buf->second->get_buffer_name(), tot);
#endif
		++buf;
	}

	// TODO
	//float4*		m_dRbForces;
	//float4*		m_dRbTorques;
	//uint*		m_dRbNum;

	// round up to next multiple of 4
	return (tot/4 + 1) * 4;
}

// Compute the bytes required for each cell.
// NOTE: this should be update for each new device array!
size_t GPUWorker::computeMemoryPerCell()
{
	size_t tot = 0;
	tot += sizeof(m_dCellStart[0]);
	tot += sizeof(m_dCellEnd[0]);
	tot += sizeof(m_dCompactDeviceMap[0]);
	return tot;
}

// Compute the maximum number of particles we can allocate according to the available device memory
void GPUWorker::computeAndSetAllocableParticles()
{
	size_t totMemory, freeMemory;
	cudaMemGetInfo(&freeMemory, &totMemory);
	freeMemory -= gdata->nGridCells * computeMemoryPerCell();
	freeMemory -= 16; // segments
	freeMemory -= 100*1024*1024; // leave 100Mb as safety margin
	if (MULTI_DEVICE)
		m_numAllocatedParticles = (freeMemory / computeMemoryPerParticle());
	else
		m_numAllocatedParticles = m_numParticles;


	if (m_numAllocatedParticles < m_numParticles) {
		printf("FATAL: thread %u needs %u particles, but there is memory for %u (plus safety margin)\n", m_deviceIndex, m_numParticles, m_numAllocatedParticles);
		exit(1);
	}
}

// Cut all particles that are not internal.
// Assuming segments have already been filled and downloaded to the shared array.
// NOTE: here it would be logical to reset the cellStarts of the cells being cropped
// out. However, this would be quite inefficient. We leave them inconsistent for a
// few time and we will update them when importing peer cells.
void GPUWorker::dropExternalParticles()
{
	m_particleRangeEnd =  m_numParticles = m_numInternalParticles;
	gdata->s_dSegmentsStart[m_deviceIndex][CELLTYPE_OUTER_EDGE_CELL] == EMPTY_SEGMENT;
	gdata->s_dSegmentsStart[m_deviceIndex][CELLTYPE_OUTER_CELL] == EMPTY_SEGMENT;
}

// Start an async inter-device transfer. This will be actually P2P if device can access peer memory
// (actually, since it is currently used only to import data from other devices, the dstDevice could be omitted or implicit)
void GPUWorker::peerAsyncTransfer(void* dst, int  dstDevice, const void* src, int  srcDevice, size_t count)
{
	CUDA_SAFE_CALL_NOSYNC( cudaMemcpyPeerAsync(	dst, dstDevice, src, srcDevice, count, m_asyncPeerCopiesStream ) );
}

// Uploads cellStart and cellEnd from the shared arrays to the device memory.
// Parameters: fromCell is inclusive, toCell is exclusive
void GPUWorker::asyncCellIndicesUpload(uint fromCell, uint toCell)
{
	uint numCells = toCell - fromCell;
	CUDA_SAFE_CALL_NOSYNC(cudaMemcpyAsync(	(m_dCellStart + fromCell),
										(gdata->s_dCellStarts[m_deviceIndex] + fromCell),
										sizeof(uint) * numCells, cudaMemcpyHostToDevice, m_asyncH2DCopiesStream));
	CUDA_SAFE_CALL_NOSYNC(cudaMemcpyAsync(	(m_dCellEnd + fromCell),
										(gdata->s_dCellEnds[m_deviceIndex] + fromCell),
										sizeof(uint) * numCells, cudaMemcpyHostToDevice, m_asyncH2DCopiesStream));
}

// Import the external edge cells of other devices to the self device arrays. Can append the cells at the end of the current
// list of particles (APPEND_EXTERNAL) or just update the already appended ones (UPDATE_EXTERNAL), according to the current
// command. When appending, also update cellStarts (device and host), cellEnds (device and host) and segments (host only).
// The arrays to be imported must be specified in the command flags. Currently supports pos, vel, info, forces and tau; for the
// double buffered arrays, it is mandatory to specify also the buffer to be used (read or write). This information is ignored
// for the non-buffered arrays (e.g. forces).
// The data is transferred in bursts of consecutive cells when possible. Transfers are actually D2D if peer access is enabled.
// Update: supports also BUFFER_BOUNDELEMENTS, BUFFER_GRADGAMMA, BUFFER_VERTICES, BUFFER_PRESSURE,
// BUFFER_TKE, BUFFER_EPSILON, BUFFER_TURBVISC, BUFFER_STRAIN_RATE
void GPUWorker::importPeerEdgeCells()
{
	// if next command is not an import nor an append, something wrong is going on
	if (! ( (gdata->nextCommand == APPEND_EXTERNAL) || (gdata->nextCommand == UPDATE_EXTERNAL) ) ) {
		printf("WARNING: importPeerEdgeCells() was called, but current command is not APPEND nor UPDATE!\n");
		return;
	}

	// check if at least one double buffer was specified
	bool dbl_buffer_specified = ( (gdata->commandFlags & DBLBUFFER_READ ) || (gdata->commandFlags & DBLBUFFER_WRITE) );
	uint dbl_buf_idx;

	// We aim to make the fewest possible transfers. So we keep track of each burst of consecutive
	// cells from the same peer device, to transfer it with a single memcpy.
	// To this aim, it is important that we iterate on the linearized index so that consecutive
	// cells are also consecutive in memory, regardless the linearization function

	// aux var for transfers
	size_t _size;

	// indices of current burst
	uint burst_self_index_begin = 0;
	uint burst_peer_index_begin = 0;
	uint burst_peer_index_end = 0;
	uint burst_numparts = 0; // this is redundant with burst_peer_index_end, but cleaner
	uint burst_peer_dev_index = 0;

	// utility defines to handle the bursts
#define BURST_IS_EMPTY (burst_numparts == 0)
#define BURST_SET_CURRENT_CELL \
	burst_self_index_begin = selfCellStart; \
	burst_peer_index_begin = peerCellStart; \
	burst_peer_index_end = peerCellEnd; \
	burst_numparts = numPartsInPeerCell; \
	burst_peer_dev_index = peerDevIndex;

	// Burst of cells (for cellStart / cellEnd). Please not the burst of cells is independent from the one of cells.
	// More specifically, it is a superset, since we are also interested in empty cells; for code readability reasons
	// we handle it as if it were completely independent.
	uint cellsBurst_begin = 0;
	uint cellsBurst_end = 0;
#define CELLS_BURST_IS_EMPTY (cellsBurst_end - cellsBurst_begin == 0)

	// iterate on all cells
	for (uint cell=0; cell < m_nGridCells; cell++)

		// if the current is an external edge cell and it belongs to a device of the same process...
		if (m_hCompactDeviceMap[cell] == CELLTYPE_OUTER_EDGE_CELL_SHIFTED && gdata->RANK(gdata->s_hDeviceMap[cell]) == gdata->mpi_rank) {

			// handle the cell burst: as long as it is OUTER_EDGE and we are appending, we need to copy it, also if empty
			if (gdata->nextCommand == APPEND_EXTERNAL) {
				// 1st case: burst is empty, let's initialize it
				if (CELLS_BURST_IS_EMPTY) {
					cellsBurst_begin = cell;
					cellsBurst_end = cell + 1;
				} else
				// 2nd case: burst is not emtpy but ends with current cell; let's append the cell
				if (cellsBurst_end == cell) {
					cellsBurst_end++;
				} else { // 3rd case: the burst is not emtpy and not compatible; need to flush it and make it new
					// this will also transfer cellEnds for empty cells but this is not a problem since we won't use that
					asyncCellIndicesUpload(cellsBurst_begin, cellsBurst_end);
					cellsBurst_begin = cell;
					cellsBurst_end = cell + 1;
				}
			}

			// check in which device it is
			uchar peerDevIndex = gdata->DEVICE( gdata->s_hDeviceMap[cell] );
			if (peerDevIndex == m_deviceIndex)
				printf("WARNING: cell %u is outer edge for thread %u, but SELF in the device map!\n", cell, m_deviceIndex);
			if (peerDevIndex >= gdata->devices) {
				printf("FATAL: cell %u has peer index %u, probable memory corruption\n", cell, peerDevIndex);
				gdata->quit_request = true;
				return;
			}
			uint peerCudaDevNum = gdata->device[peerDevIndex];

			// find its cellStart and cellEnd
			uint peerCellStart = gdata->s_dCellStarts[peerDevIndex][cell];
			uint peerCellEnd = gdata->s_dCellEnds[peerDevIndex][cell];

			// cellStart and cellEnd on self
			uint selfCellStart;
			uint selfCellEnd;

			// if it is empty, we might only update cellStarts
			if (peerCellStart == EMPTY_CELL) {

				if (gdata->nextCommand == APPEND_EXTERNAL) {
					// set the cell as empty
					gdata->s_dCellStarts[m_deviceIndex][cell] = EMPTY_CELL;
				}

			} else {
				// cellEnd is exclusive
				uint numPartsInPeerCell = peerCellEnd - peerCellStart;

				if (gdata->nextCommand == UPDATE_EXTERNAL) {
					// if updating, we already have cell indices
					selfCellStart = gdata->s_dCellStarts[m_deviceIndex][cell];
					selfCellEnd = gdata->s_dCellEnds[m_deviceIndex][cell];
				} else {
					// otherwise, we are importing and cell is being appended at current numParts
					selfCellStart = m_numParticles;
					selfCellEnd = m_numParticles + numPartsInPeerCell;
				}

				if (gdata->nextCommand == APPEND_EXTERNAL) {
					// Only when appending, we also need to update cellStarts and cellEnds, both on host (for next updatee) and on device (for accessing cells).
					// Now we write
					//   cellStart[cell] = m_numParticles
					//   cellEnd[cell] = m_numParticles + numPartsInPeerCell
					// in both device memory (used in neib search) and host buffers (partially used in next update: if
					// only the receiving device reads them, they are not used).

					// Update host copy of cellStart and cellEnd. Since it is an external cell,
					// it is unlikely that the host copy will be used, but it is always good to keep
					// indices consistent. The device copy is being updated throught the burst mechanism.
					gdata->s_dCellStarts[m_deviceIndex][cell] = selfCellStart;
					gdata->s_dCellEnds[m_deviceIndex][cell] = selfCellEnd;

					// Also update outer edge segment, if it was empty.
					// NOTE: keeping correctness only if there are no OUTER particles (which we assume)
					if (gdata->s_dSegmentsStart[m_deviceIndex][CELLTYPE_OUTER_EDGE_CELL] == EMPTY_SEGMENT)
						gdata->s_dSegmentsStart[m_deviceIndex][CELLTYPE_OUTER_EDGE_CELL] = selfCellStart;

					// finally, update the total number of particles
					m_numParticles += numPartsInPeerCell;
				}

				// now we deal with the actual data in the cells

				if (BURST_IS_EMPTY) {

					// no burst yet; initialize with current cell
					BURST_SET_CURRENT_CELL

				} else
				if (burst_peer_dev_index == peerDevIndex && burst_peer_index_end == peerCellStart) {

					// previous burst is compatible with current cell: extend it
					burst_peer_index_end = peerCellEnd;
					burst_numparts += numPartsInPeerCell;

				} else {
					// Previous burst is not compatible, need to flush the "buffer" and reset it to the current cell.

					// iterate over all defined buffers and see which were requested
					// NOTE: std::map, from which BufferList is derived, is an _ordered_ container,
					// with the ordering set by the key, in our case the unsigned integer type flag_t,
					// so we have guarantee that the map will always be traversed in the same order
					// (unless stuff is inserted/deleted, which shouldn't happen at program runtime)
					BufferList::iterator bufset = m_dBuffers.begin();
					const BufferList::iterator stop = m_dBuffers.end();
					for ( ; bufset != stop ; ++bufset) {
						flag_t bufkey = bufset->first;
						if (!(gdata->commandFlags & bufkey))
							continue; // skip unwanted buffers

						AbstractBuffer *dstbuf = bufset->second;

						// handling of double-buffered arrays
						// note that TAU is not considered here
						if (dstbuf->get_array_count() == 2) {
							// for buffers with more than one array the caller should have specified which buffer
							// is to be imported. complain
							if (!dbl_buffer_specified) {
								std::stringstream err_msg;
								err_msg << "Import request for double-buffered " << dstbuf->get_buffer_name()
									<< " array without a specification of which buffer to use.";
								throw runtime_error(err_msg.str());
							}

							if (gdata->commandFlags & DBLBUFFER_READ)
								dbl_buf_idx = gdata->currentRead[bufkey];
							else
								dbl_buf_idx = gdata->currentWrite[bufkey];
						} else {
							dbl_buf_idx = 0;
						}

						const AbstractBuffer *srcbuf = gdata->GPUWORKERS[burst_peer_dev_index]->getBuffer(bufkey);
						_size = burst_numparts * dstbuf->get_element_size();

						// special treatment for TAU, since in that case we need to transfers all 3 arrays
						if (bufkey != BUFFER_TAU) {
							void *dstptr = dstbuf->get_offset_buffer(dbl_buf_idx, burst_self_index_begin);
							const void *srcptr = srcbuf->get_offset_buffer(dbl_buf_idx, burst_peer_index_begin);

							peerAsyncTransfer(dstptr, m_cudaDeviceNumber, srcptr, burst_peer_dev_index, _size);
						} else {
							// generic, so that it can work for other buffers like TAU, if they are ever
							// introduced; just fix the conditional
							for (uint ai = 0; ai < dstbuf->get_array_count(); ++ai) {
								void *dstptr = dstbuf->get_offset_buffer(ai, burst_self_index_begin);
								const void *srcptr = srcbuf->get_offset_buffer(ai, burst_peer_index_begin);
								peerAsyncTransfer(dstptr, m_cudaDeviceNumber, srcptr, burst_peer_dev_index, _size);
							}
						}
					}

					// reset burst to current cell
					BURST_SET_CURRENT_CELL
				} // burst flush

			} // if cell is not empty
		} // if cell is external edge and in the same node

	// flush the burst if not empty (should always happen if at least one edge cell is not empty)
	if (!BURST_IS_EMPTY) {

		// iterate over all defined buffers and see which were requested
		// see NOTE above about std::map traversal order
		// TODO it seems this is exactly the same code as above. Refactor?
		BufferList::iterator bufset = m_dBuffers.begin();
		const BufferList::iterator stop = m_dBuffers.end();
		for ( ; bufset != stop ; ++bufset) {
			flag_t bufkey = bufset->first;
			if (!(gdata->commandFlags & bufkey))
				continue; // skip unwanted buffers

			AbstractBuffer *dstbuf = bufset->second;

			// handling of double-buffered arrays
			if (dstbuf->get_array_count() > 1) {
				// for buffers with more than one array the caller should have specified which buffer
				// is to be imported. complain
				if (!dbl_buffer_specified) {
					std::stringstream err_msg;
					err_msg << "Import request for double-buffered " << dstbuf->get_buffer_name()
						<< " array without a specification of which buffer to use.";
					throw runtime_error(err_msg.str());
				}

				if (gdata->commandFlags & DBLBUFFER_READ)
					dbl_buf_idx = gdata->currentRead[bufkey];
				else
					dbl_buf_idx = gdata->currentWrite[bufkey];
			} else {
				dbl_buf_idx = 0;
			}

			const AbstractBuffer *srcbuf = gdata->GPUWORKERS[burst_peer_dev_index]->getBuffer(bufkey);
			_size = burst_numparts * dstbuf->get_element_size();

			// special treatment for TAU, since in that case we need to transfers all 3 arrays
			if (bufkey != BUFFER_TAU) {
				void *dstptr = dstbuf->get_offset_buffer(dbl_buf_idx, burst_self_index_begin);
				const void *srcptr = srcbuf->get_offset_buffer(dbl_buf_idx, burst_peer_index_begin);

				peerAsyncTransfer(dstptr, m_cudaDeviceNumber, srcptr, burst_peer_dev_index, _size);
			} else {
				// generic, so that it can work for other buffers like TAU, if they are ever
				// introduced; just fix the conditional
				for (uint ai = 0; ai < dstbuf->get_array_count(); ++ai) {
					void *dstptr = dstbuf->get_offset_buffer(ai, burst_self_index_begin);
					const void *srcptr = srcbuf->get_offset_buffer(ai, burst_peer_index_begin);
					peerAsyncTransfer(dstptr, m_cudaDeviceNumber, srcptr, burst_peer_dev_index, _size);
				}
			}
		}
	} // burst is empty?

	// also flush cell buffer, if any
	if (gdata->nextCommand == APPEND_EXTERNAL && !CELLS_BURST_IS_EMPTY)
		asyncCellIndicesUpload(cellsBurst_begin, cellsBurst_end);

#undef BURST_IS_EMPTY
#undef BURST_SET_CURRENT_CELL
#undef CELLS_BURST_IS_EMPTY

	// cudaMemcpyPeerAsync() is asynchronous with the host. We synchronize at the end to wait for the
	// transfers to be complete.
	cudaDeviceSynchronize();
}

// Import the external edge cells of other nodes to the self device arrays. Can append the cells at the end of the current
// list of particles (APPEND_EXTERNAL) or just update the already appended ones (UPDATE_EXTERNAL), according to the current
// command. When appending, also update cellStarts (device and host), cellEnds (device and host) and segments (host only).
// The arrays to be imported must be specified in the command flags. Currently supports pos, vel, info, forces and tau; for the
// double buffered arrays, it is mandatory to specify also the buffer to be used (read or write). This information is ignored
// for the non-buffered arrays (e.g. forces).
// The data is transferred in bursts of consecutive cells when possible.
// Update: supports also BUFFER_BOUNDELEMENTS, BUFFER_GRADGAMMA, BUFFER_VERTICES, BUFFER_PRESSURE,
// BUFFER_TKE, BUFFER_EPSILON, BUFFER_TURBVISC, BUFFER_STRAIN_RATE
void GPUWorker::importNetworkPeerEdgeCells()
{
	// if next command is not an import nor an append, something wrong is going on
	if (! ( (gdata->nextCommand == APPEND_EXTERNAL) || (gdata->nextCommand == UPDATE_EXTERNAL) ) ) {
		printf("WARNING: importNetworkPeerEdgeCells() was called, but current command is not APPEND nor UPDATE!\n");
		return;
	}

	// is a double buffer specified?
	bool dbl_buffer_specified = ( (gdata->commandFlags & DBLBUFFER_READ ) || (gdata->commandFlags & DBLBUFFER_WRITE) );
	uint dbl_buf_idx;

	// We need to import every cell of the neigbor processes only once. To this aim, we keep a list of recipient
	// ranks who already received the current cell. The list, in form of a bitmap, is reset before iterating on
	// all the neighbor cells
	bool recipient_devices[MAX_DEVICES_PER_CLUSTER];

	// Unlike importing from other devices in the same process, here we need one burst for each potential neighbor device;
	// moreover, we only need the "self" address
	uint burst_self_index_begin[MAX_DEVICES_PER_CLUSTER];
	uint burst_self_index_end[MAX_DEVICES_PER_CLUSTER];
	uint burst_numparts[MAX_DEVICES_PER_CLUSTER]; // redundant with burst_peer_index_end, but cleaner
	bool burst_is_sending[MAX_DEVICES_PER_CLUSTER]; // true for bursts of sends, false for bursts of receives

	// initialize bursts
	for (uint n = 0; n < MAX_DEVICES_PER_CLUSTER; n++) {
		burst_self_index_begin[n] = 0;
		burst_self_index_end[n] = 0;
		burst_numparts[n] = 0;
		burst_is_sending[n] = false;
	}

	// utility defines to handle the bursts
#define BURST_IS_EMPTY (burst_numparts[otherDeviceGlobalDevIdx] == 0)
#define BURST_SET_CURRENT_CELL \
	burst_self_index_begin[otherDeviceGlobalDevIdx] = curr_cell_start; \
	burst_self_index_end[otherDeviceGlobalDevIdx] = curr_cell_start + partsInCurrCell; \
	burst_numparts[otherDeviceGlobalDevIdx] = partsInCurrCell; \
	burst_is_sending[otherDeviceGlobalDevIdx] = is_sending;

	// While we iterate on the cells we refer as "curr" to the cell indexed by the outer cycles and as "neib"
	// to the neib cell indexed by the inner cycles. Either cell could belong to current process (so the bools
	// curr_mine and neib_mine); one should not think that neib_cell is necessary in a neib device or process.
	// Instead, otherDeviceGlobalDevIdx, which is used to handle the bursts, is always the global index of the
	// "other" device.

	// iterate on all cells
	for (uint lin_curr_cell = 0; lin_curr_cell < m_nGridCells; lin_curr_cell++) {

		// we will need the 3D coords as well
		int3 curr_cell = gdata->reverseGridHashHost(lin_curr_cell);

		// optimization: if not edging, continue
		if (m_hCompactDeviceMap[lin_curr_cell] == CELLTYPE_OUTER_CELL_SHIFTED ||
			m_hCompactDeviceMap[lin_curr_cell] == CELLTYPE_INNER_CELL_SHIFTED ) continue;

		// reset the list for recipient neib processes
		for (uint d = 0; d < MAX_DEVICES_PER_CLUSTER; d++)
			recipient_devices[d] = false;

		uint curr_cell_globalDevIdx = gdata->s_hDeviceMap[lin_curr_cell];
		uchar curr_cell_rank = gdata->RANK( curr_cell_globalDevIdx );
		if ( curr_cell_rank >= gdata->mpi_nodes ) {
			printf("FATAL: cell %u seems to belong to rank %u, but max is %u; probable memory corruption\n", lin_curr_cell, curr_cell_rank, gdata->mpi_nodes - 1);
			gdata->quit_request = true;
			return;
		}

		bool curr_mine = (curr_cell_globalDevIdx == m_globalDeviceIdx);

		// iterate on neighbors
		for (int dz = -1; dz <= 1; dz++)
			for (int dy = -1; dy <= 1; dy++)
				for (int dx = -1; dx <= 1; dx++) {

					// skip self (should be implicit with dev id check, later)
					if (dx == 0 && dy == 0 && dz == 0) continue;

					// ensure we are inside the grid
					if (curr_cell.x + dx < 0 || curr_cell.x + dx >= gdata->gridSize.x) continue;
					if (curr_cell.y + dy < 0 || curr_cell.y + dy >= gdata->gridSize.y) continue;
					if (curr_cell.z + dz < 0 || curr_cell.z + dz >= gdata->gridSize.z) continue;

					// linearized hash of neib cell
					uint lin_neib_cell = gdata->calcGridHashHost(curr_cell.x + dx, curr_cell.y + dy, curr_cell.z + dz);
					uint neib_cell_globalDevIdx = gdata->s_hDeviceMap[lin_neib_cell];

					// will be set in different way depending on the rank (mine, then local cellStarts, or not, then receive size via network)
					uint partsInCurrCell;
					uint curr_cell_start;

					bool neib_mine = (neib_cell_globalDevIdx == m_globalDeviceIdx);
					uchar neib_cell_rank = gdata->RANK( neib_cell_globalDevIdx );

					// global dev idx of the "other" device
					uint otherDeviceGlobalDevIdx = (curr_cell_globalDevIdx == m_globalDeviceIdx ? neib_cell_globalDevIdx : curr_cell_globalDevIdx);
					bool is_sending = curr_mine;

					// did we already treat the pair (curr_rank <-> neib_rank) for this cell?
					if (recipient_devices[ gdata->GLOBAL_DEVICE_NUM(neib_cell_globalDevIdx) ]) continue;

					// we can skip 1. pairs belonging to the same node 2. pairs where not current nor neib belong to current process
					if (curr_cell_rank == neib_cell_rank || ( !curr_mine && !neib_mine ) ) continue;

					// initialize the number of particles in the cell
					partsInCurrCell = 0;

					// if the cell belong to a neib node and we are appending, there are a few things to handle
					if (neib_mine && gdata->nextCommand == APPEND_EXTERNAL) {

						// prepare to append it to the end of the present array
						curr_cell_start = m_numParticles;

						// receive the size from the cell process
						gdata->networkManager->receiveUint(curr_cell_globalDevIdx, neib_cell_globalDevIdx, &partsInCurrCell);

						// update host cellStarts/Ends
						if (partsInCurrCell > 0) {
							gdata->s_dCellStarts[m_deviceIndex][lin_curr_cell] = curr_cell_start;
							gdata->s_dCellEnds[m_deviceIndex][lin_curr_cell] = curr_cell_start + partsInCurrCell;
						} else
							gdata->s_dCellStarts[m_deviceIndex][lin_curr_cell] = EMPTY_CELL;

						// update device cellStarts/Ends
						CUDA_SAFE_CALL_NOSYNC(cudaMemcpy( (m_dCellStart + lin_curr_cell), (gdata->s_dCellStarts[m_deviceIndex] + lin_curr_cell),
							sizeof(uint), cudaMemcpyHostToDevice));
						if (partsInCurrCell > 0)
							CUDA_SAFE_CALL_NOSYNC(cudaMemcpy( (m_dCellEnd + lin_curr_cell), (gdata->s_dCellEnds[m_deviceIndex] + lin_curr_cell),
								sizeof(uint), cudaMemcpyHostToDevice));

						// update outer edge segment
						if (gdata->s_dSegmentsStart[m_deviceIndex][CELLTYPE_OUTER_EDGE_CELL] == EMPTY_SEGMENT && partsInCurrCell > 0)
							gdata->s_dSegmentsStart[m_deviceIndex][CELLTYPE_OUTER_EDGE_CELL] = m_numParticles;

						// update the total number of particles
						m_numParticles += partsInCurrCell;
					} // we could "else" here and totally delete the condition (it is opposite to the previuos, but left for clarity)

					// if current cell is mine or if we already imported it and we just need to update, read its size from cellStart/End
					if (curr_mine || gdata->nextCommand == UPDATE_EXTERNAL) {
						// read the size
						curr_cell_start = gdata->s_dCellStarts[m_deviceIndex][lin_curr_cell];

						// set partsInCurrCell
						if (curr_cell_start != EMPTY_CELL)
							partsInCurrCell = gdata->s_dCellEnds[m_deviceIndex][lin_curr_cell] - curr_cell_start;
					}

					// finally, if the cell belongs to current process and we are in appending phase, we want to send its size to the neib process
					if (curr_mine && gdata->nextCommand == APPEND_EXTERNAL)
						gdata->networkManager->sendUint(curr_cell_globalDevIdx, neib_cell_globalDevIdx, &partsInCurrCell);

					// mark the pair current_cell:neib_node as treated
					recipient_devices[ gdata->GLOBAL_DEVICE_NUM(neib_cell_globalDevIdx) ] = true;

					// if the cell is empty, just continue to next one
					if  (curr_cell_start == EMPTY_CELL) continue;

					if (BURST_IS_EMPTY) {

						// burst is empty, so create a new one and continue
						BURST_SET_CURRENT_CELL

					} else
					// condition: curr cell begins when burst end AND burst is in the same direction (send / receive)
					if (burst_self_index_end[otherDeviceGlobalDevIdx] == curr_cell_start && burst_is_sending[otherDeviceGlobalDevIdx] == is_sending) {

						// cell is compatible, extend the burst
						burst_self_index_end[otherDeviceGlobalDevIdx] += partsInCurrCell;
						burst_numparts[otherDeviceGlobalDevIdx] += partsInCurrCell;

					} else {

						// the burst for the current "other" node was non-empty and not compatible; flush it and make a new one

						// Now partsInCurrCell and curr_cell_start are ready; let's handle the actual transfers.

						if ( burst_is_sending[otherDeviceGlobalDevIdx] ) {
							// Current is mine: send the cells to the process holding the neighbor cells
							// (in the following, m_globalDeviceIdx === curr_cell_globalDevIdx)

							// iterate over all defined buffers and see which were requested
							// NOTE: std::map, from which BufferList is derived, is an _ordered_ container,
							// with the ordering set by the key, in our case the unsigned integer type flag_t,
							// so we have guarantee that the map will always be traversed in the same order
							// (unless stuff is inserted/deleted, which shouldn't happen at program runtime)
							BufferList::iterator bufset = m_dBuffers.begin();
							const BufferList::iterator stop = m_dBuffers.end();
							for ( ; bufset != stop ; ++bufset) {
								flag_t bufkey = bufset->first;
								if (!(gdata->commandFlags & bufkey))
									continue; // skip unwanted buffers

								AbstractBuffer *srcbuf = bufset->second;

								// handling of double-buffered arrays
								// note that TAU is not considered here
								if (srcbuf->get_array_count() == 2) {
									// for buffers with more than one array the caller should have specified which buffer
									// is to be imported. complain
									if (!dbl_buffer_specified) {
										std::stringstream err_msg;
										err_msg << "Import request for double-buffered " << srcbuf->get_buffer_name()
											<< " array without a specification of which buffer to use.";
										throw runtime_error(err_msg.str());
									}

									if (gdata->commandFlags & DBLBUFFER_READ)
										dbl_buf_idx = gdata->currentRead[bufkey];
									else
										dbl_buf_idx = gdata->currentWrite[bufkey];
								} else {
									dbl_buf_idx = 0;
								}

								unsigned int _size = burst_numparts[otherDeviceGlobalDevIdx] * srcbuf->get_element_size();

								// special treatment for TAU, since in that case we need to transfers all 3 arrays
								if (bufkey != BUFFER_TAU) {
									void *srcptr = srcbuf->get_offset_buffer(dbl_buf_idx, burst_self_index_begin[otherDeviceGlobalDevIdx]);
									gdata->networkManager->sendBuffer(m_globalDeviceIdx, otherDeviceGlobalDevIdx, _size, srcptr);
								} else {
									// generic, so that it can work for other buffers like TAU, if they are ever
									// introduced; just fix the conditional
									for (uint ai = 0; ai < srcbuf->get_array_count(); ++ai) {
										void *srcptr = srcbuf->get_offset_buffer(ai, burst_self_index_begin[otherDeviceGlobalDevIdx]);
										gdata->networkManager->sendBuffer(m_globalDeviceIdx, otherDeviceGlobalDevIdx, _size, srcptr);
									}
								}
							}
						} else {
							// neighbor is mine: receive the cells from the process holding the current cell
							// (in the following, m_globalDeviceIdx === neib_cell_globalDevIdx)

							// iterate over all defined buffers and see which were requested
							// NOTE: std::map, from which BufferList is derived, is an _ordered_ container,
							// with the ordering set by the key, in our case the unsigned integer type flag_t,
							// so we have guarantee that the map will always be traversed in the same order
							// (unless stuff is inserted/deleted, which shouldn't happen at program runtime)
							BufferList::iterator bufset = m_dBuffers.begin();
							const BufferList::iterator stop = m_dBuffers.end();
							for ( ; bufset != stop ; ++bufset) {
								flag_t bufkey = bufset->first;
								if (!(gdata->commandFlags & bufkey))
									continue; // skip unwanted buffers

								AbstractBuffer *dstbuf = bufset->second;

								// handling of double-buffered arrays
								// note that TAU is not considered here
								if (dstbuf->get_array_count() == 2) {
									// for buffers with more than one array the caller should have specified which buffer
									// is to be imported. complain
									if (!dbl_buffer_specified) {
										std::stringstream err_msg;
										err_msg << "Import request for double-buffered " << dstbuf->get_buffer_name()
											<< " array without a specification of which buffer to use.";
										throw runtime_error(err_msg.str());
									}

									if (gdata->commandFlags & DBLBUFFER_READ)
										dbl_buf_idx = gdata->currentRead[bufkey];
									else
										dbl_buf_idx = gdata->currentWrite[bufkey];
								} else {
									dbl_buf_idx = 0;
								}

								unsigned int _size = burst_numparts[otherDeviceGlobalDevIdx] * dstbuf->get_element_size();

								// special treatment for TAU, since in that case we need to transfers all 3 arrays
								if (bufkey != BUFFER_TAU) {
									void *dstptr = dstbuf->get_offset_buffer(dbl_buf_idx, burst_self_index_begin[otherDeviceGlobalDevIdx]);
									gdata->networkManager->receiveBuffer(otherDeviceGlobalDevIdx, m_globalDeviceIdx, _size, dstptr);
								} else {
									// generic, so that it can work for other buffers like TAU, if they are ever
									// introduced; just fix the conditional
									for (uint ai = 0; ai < dstbuf->get_array_count(); ++ai) {
										void *dstptr = dstbuf->get_offset_buffer(ai, burst_self_index_begin[otherDeviceGlobalDevIdx]);
										gdata->networkManager->receiveBuffer(otherDeviceGlobalDevIdx, m_globalDeviceIdx, _size, dstptr);
									}
								}
							}
						}

						BURST_SET_CURRENT_CELL
					} // end of flushing and resetting the burst

				} // iterate on neighbor cells
	} // iterate on cells

	// here: flush all the non-empty bursts
	for (uint otherDevGlobalIdx = 0; otherDevGlobalIdx < MAX_DEVICES_PER_CLUSTER; otherDevGlobalIdx++)
		if ( burst_numparts[otherDevGlobalIdx] > 0) {

			if ( burst_is_sending[otherDevGlobalIdx] ) {
				// Current is mine: send the cells to the process holding the neighbor cells

				// iterate over all defined buffers and see which were requested
				// NOTE: std::map, from which BufferList is derived, is an _ordered_ container,
				// with the ordering set by the key, in our case the unsigned integer type flag_t,
				// so we have guarantee that the map will always be traversed in the same order
				// (unless stuff is inserted/deleted, which shouldn't happen at program runtime)
				BufferList::iterator bufset = m_dBuffers.begin();
				const BufferList::iterator stop = m_dBuffers.end();
				for ( ; bufset != stop ; ++bufset) {
					flag_t bufkey = bufset->first;
					if (!(gdata->commandFlags & bufkey))
						continue; // skip unwanted buffers

					AbstractBuffer *srcbuf = bufset->second;

					// handling of double-buffered arrays
					// note that TAU is not considered here
					if (srcbuf->get_array_count() == 2) {
						// for buffers with more than one array the caller should have specified which buffer
						// is to be imported. complain
						if (!dbl_buffer_specified) {
							std::stringstream err_msg;
							err_msg << "Import request for double-buffered " << srcbuf->get_buffer_name()
								<< " array without a specification of which buffer to use.";
							throw runtime_error(err_msg.str());
						}

						if (gdata->commandFlags & DBLBUFFER_READ)
							dbl_buf_idx = gdata->currentRead[bufkey];
						else
							dbl_buf_idx = gdata->currentWrite[bufkey];
					} else {
						dbl_buf_idx = 0;
					}

					unsigned int _size = burst_numparts[otherDevGlobalIdx] * srcbuf->get_element_size();

					// special treatment for TAU, since in that case we need to transfers all 3 arrays
					if (bufkey != BUFFER_TAU) {
						void *srcptr = srcbuf->get_offset_buffer(dbl_buf_idx, burst_self_index_begin[otherDevGlobalIdx]);
						gdata->networkManager->sendBuffer(m_globalDeviceIdx, otherDevGlobalIdx, _size, srcptr);
					} else {
						// generic, so that it can work for other buffers like TAU, if they are ever
						// introduced; just fix the conditional
						for (uint ai = 0; ai < srcbuf->get_array_count(); ++ai) {
							void *srcptr = srcbuf->get_offset_buffer(ai, burst_self_index_begin[otherDevGlobalIdx]);
							gdata->networkManager->sendBuffer(m_globalDeviceIdx, otherDevGlobalIdx, _size, srcptr);
						}
					}
				}
			} else {
				// neighbor is mine: receive the cells from the process holding the current cell
				// (in the following, m_globalDeviceIdx === neib_cell_globalDevIdx)

				// iterate over all defined buffers and see which were requested
				// NOTE: std::map, from which BufferList is derived, is an _ordered_ container,
				// with the ordering set by the key, in our case the unsigned integer type flag_t,
				// so we have guarantee that the map will always be traversed in the same order
				// (unless stuff is inserted/deleted, which shouldn't happen at program runtime)
				BufferList::iterator bufset = m_dBuffers.begin();
				const BufferList::iterator stop = m_dBuffers.end();
				for ( ; bufset != stop ; ++bufset) {
					flag_t bufkey = bufset->first;
					if (!(gdata->commandFlags & bufkey))
						continue; // skip unwanted buffers

					AbstractBuffer *dstbuf = bufset->second;

					// handling of double-buffered arrays
					// note that TAU is not considered here
					if (dstbuf->get_array_count() == 2) {
						// for buffers with more than one array the caller should have specified which buffer
						// is to be imported. complain
						if (!dbl_buffer_specified) {
							std::stringstream err_msg;
							err_msg << "Import request for double-buffered " << dstbuf->get_buffer_name()
								<< " array without a specification of which buffer to use.";
							throw runtime_error(err_msg.str());
						}

						if (gdata->commandFlags & DBLBUFFER_READ)
							dbl_buf_idx = gdata->currentRead[bufkey];
						else
							dbl_buf_idx = gdata->currentWrite[bufkey];
					} else {
						dbl_buf_idx = 0;
					}

					unsigned int _size = burst_numparts[otherDevGlobalIdx] * dstbuf->get_element_size();

					// special treatment for TAU, since in that case we need to transfers all 3 arrays
					if (bufkey != BUFFER_TAU) {
						void *dstptr = dstbuf->get_offset_buffer(dbl_buf_idx, burst_self_index_begin[otherDevGlobalIdx]);
						gdata->networkManager->receiveBuffer(otherDevGlobalIdx, m_globalDeviceIdx, _size, dstptr);
					} else {
						// generic, so that it can work for other buffers like TAU, if they are ever
						// introduced; just fix the conditional
						for (uint ai = 0; ai < dstbuf->get_array_count(); ++ai) {
							void *dstptr = dstbuf->get_offset_buffer(ai, burst_self_index_begin[otherDevGlobalIdx]);
							gdata->networkManager->receiveBuffer(otherDevGlobalIdx, m_globalDeviceIdx, _size, dstptr);
						}
					}
				}
			}

		}

	// don't need the defines anymore
#undef BURST_IS_EMPTY
#undef BURST_SET_CURRENT_CELL

}

// All the allocators assume that gdata is updated with the number of particles (done by problem->fillparts).
// Later this will be changed since each thread does not need to allocate the global number of particles.
size_t GPUWorker::allocateHostBuffers() {
	// common sizes
	const uint float3Size = sizeof(float3) * m_numAllocatedParticles;
	const uint float4Size = sizeof(float4) * m_numAllocatedParticles;
	const uint infoSize = sizeof(particleinfo) * m_numAllocatedParticles;
	const uint uintCellsSize = sizeof(uint) * m_nGridCells;

	size_t allocated = 0;

	/*m_hPos = new float4[m_numAllocatedParticles];
	memset(m_hPos, 0, float4Size);
	allocated += float4Size;

	m_hVel = new float4[m_numAllocatedParticles];
	memset(m_hVel, 0, float4Size);
	allocated += float4Size;

	m_hInfo = new particleinfo[m_numAllocatedParticles];
	memset(m_hInfo, 0, infoSize);
	allocated += infoSize; */

	if (MULTI_DEVICE) {
		m_hCompactDeviceMap = new uint[m_nGridCells];
		memset(m_hCompactDeviceMap, 0, uintCellsSize);
		allocated += uintCellsSize;
	}

	/*if (m_simparams->vorticity) {
		m_hVort = new float3[m_numAllocatedParticles];
		allocated += float3Size;
		// NOTE: *not* memsetting, as in master branch
	}*/

	m_hostMemory += allocated;
	return allocated;
}

size_t GPUWorker::allocateDeviceBuffers() {
	// common sizes
	// compute common sizes (in bytes)

	const size_t uintCellsSize = sizeof(uint) * m_nGridCells;
	const size_t segmentsSize = sizeof(uint) * 4; // 4 = types of cells

	size_t allocated = 0;

	// used to set up the number of elements in CFL arrays,
	// will only actually be used if adaptive timestepping is enabled

	uint fmaxElements = getFmaxElements(m_numAllocatedParticles);
	uint tempCflEls = getFmaxTempElements(fmaxElements);
	BufferList::iterator iter = m_dBuffers.begin();
	while (iter != m_dBuffers.end()) {
		// number of elements to allocate
		// most have m_numAllocatedParticles. Exceptions follow
		size_t nels = m_numAllocatedParticles;

		// iter->first: the key
		// iter->second: the Buffer
		if (iter->first & BUFFER_NEIBSLIST)
			nels *= m_simparams->maxneibsnum; // number of particles times max neibs num
		else if (iter->first & BUFFER_CFL_TEMP)
			nels = tempCflEls;
		else if (iter->first & BUFFERS_CFL) // other CFL buffers
			nels = fmaxElements;

		allocated += iter->second->alloc(nels);
		++iter;
	}

	CUDA_SAFE_CALL(cudaMalloc(&m_dCellStart, uintCellsSize));
	allocated += uintCellsSize;

	CUDA_SAFE_CALL(cudaMalloc(&m_dCellEnd, uintCellsSize));
	allocated += uintCellsSize;

	// TODO: an array of uchar would suffice
	CUDA_SAFE_CALL(cudaMalloc(&m_dCompactDeviceMap, uintCellsSize));
	// initialize anyway for single-GPU simulations
	CUDA_SAFE_CALL(cudaMemset(m_dCompactDeviceMap, 0, uintCellsSize));
	allocated += uintCellsSize;

	CUDA_SAFE_CALL(cudaMalloc(&m_dSegmentStart, segmentsSize));
	// ditto
	CUDA_SAFE_CALL(cudaMemset(m_dSegmentStart, 0, segmentsSize));
	allocated += segmentsSize;

	if (m_simparams->numODEbodies) {
		m_numBodiesParticles = gdata->problem->get_ODE_bodies_numparts();
		printf("number of rigid bodies particles = %d\n", m_numBodiesParticles);

		int objParticlesFloat4Size = m_numBodiesParticles*sizeof(float4);
		int objParticlesUintSize = m_numBodiesParticles*sizeof(uint);

		CUDA_SAFE_CALL(cudaMalloc(&m_dRbTorques, objParticlesFloat4Size));
		CUDA_SAFE_CALL(cudaMalloc(&m_dRbForces, objParticlesFloat4Size));
		CUDA_SAFE_CALL(cudaMalloc(&m_dRbNum, objParticlesUintSize));

		allocated += 2 * objParticlesFloat4Size + objParticlesUintSize;

		// DEBUG
		// m_hRbForces = new float4[m_numBodiesParticles];
		// m_hRbTorques = new float4[m_numBodiesParticles];

		uint rbfirstindex[MAXBODIES];
		uint* rbnum = new uint[m_numBodiesParticles];

		rbfirstindex[0] = 0;
		for (uint i = 1; i < m_simparams->numODEbodies; i++) {
			rbfirstindex[i] = rbfirstindex[i - 1] + gdata->problem->get_ODE_body_numparts(i - 1);
		}
		setforcesrbstart(rbfirstindex, m_simparams->numODEbodies);

		int offset = 0;
		for (uint i = 0; i < m_simparams->numODEbodies; i++) {
			gdata->s_hRbLastIndex[i] = gdata->problem->get_ODE_body_numparts(i) - 1 + offset;

			for (int j = 0; j < gdata->problem->get_ODE_body_numparts(i); j++) {
				rbnum[offset + j] = i;
			}
			offset += gdata->problem->get_ODE_body_numparts(i);
		}
		size_t  size = m_numBodiesParticles*sizeof(uint);
		CUDA_SAFE_CALL(cudaMemcpy((void *) m_dRbNum, (void*) rbnum, size, cudaMemcpyHostToDevice));

		delete[] rbnum;
	}

	// TODO: call setDemTexture(), which allocates and reads the DEM
	//if (m_simparams->usedem) {
	//	//printf("Using DEM\n");
	//	//printf("cols = %d\trows =% d\n", m_problem->m_ncols, m_problem->m_nrows);
	//	setDemTexture(m_problem->m_dem, m_problem->m_ncols, m_problem->m_nrows);
	//}

	m_deviceMemory += allocated;
	return allocated;
}

void GPUWorker::deallocateHostBuffers() {
	//delete [] m_hPos;
	//delete [] m_hVel;
	//delete [] m_hInfo;
	if (MULTI_DEVICE)
		delete [] m_hCompactDeviceMap;
	/*if (m_simparams->vorticity)
		delete [] m_hVort;*/
	// here: dem host buffers?
}

void GPUWorker::deallocateDeviceBuffers() {

	m_dBuffers.clear();

	CUDA_SAFE_CALL(cudaFree(m_dCellStart));
	CUDA_SAFE_CALL(cudaFree(m_dCellEnd));

	CUDA_SAFE_CALL(cudaFree(m_dCompactDeviceMap));
	CUDA_SAFE_CALL(cudaFree(m_dSegmentStart));

	if (m_simparams->numODEbodies) {
		CUDA_SAFE_CALL(cudaFree(m_dRbTorques));
		CUDA_SAFE_CALL(cudaFree(m_dRbForces));
		CUDA_SAFE_CALL(cudaFree(m_dRbNum));

		// DEBUG
		// delete [] m_hRbForces;
		// delete [] m_hRbTorques;
	}


	// here: dem device buffers?
}

void GPUWorker::createStreams()
{
	cudaStreamCreate(&m_asyncD2HCopiesStream);
	cudaStreamCreate(&m_asyncH2DCopiesStream);
	cudaStreamCreate(&m_asyncPeerCopiesStream);
}

void GPUWorker::destroyStreams()
{
	// destroy streams
	cudaStreamDestroy(m_asyncD2HCopiesStream);
	cudaStreamDestroy(m_asyncH2DCopiesStream);
	cudaStreamDestroy(m_asyncPeerCopiesStream);
}

void GPUWorker::printAllocatedMemory()
{
	printf("Device idx %u (CUDA: %u) allocated %s on host, %s on device\n"
			"  assigned particles: %s; allocated: %s\n", m_deviceIndex, m_cudaDeviceNumber,
			gdata->memString(getHostMemory()).c_str(),
			gdata->memString(getDeviceMemory()).c_str(),
			gdata->addSeparators(m_numParticles).c_str(), gdata->addSeparators(m_numAllocatedParticles).c_str());
}

// upload subdomain, just allocated and sorted by main thread
void GPUWorker::uploadSubdomain() {
	// indices
	const uint firstInnerParticle	= gdata->s_hStartPerDevice[m_deviceIndex];
	const uint howManyParticles	= gdata->s_hPartsPerDevice[m_deviceIndex];

	// is the device empty? (unlikely but possible before LB kicks in)
	if (howManyParticles == 0) return;

	// buffers to skip in the upload. Rationale
	// POS_DOUBLE is computed on host from POS and HASH
	// NORMALS and VORTICITY are post-processing, so always produced on device
	// and _downloaded_ to host, never uploaded
	static const flag_t skip_bufs = BUFFER_POS_DOUBLE |
		BUFFER_NORMALS | BUFFER_VORTICITY;

	// iterate over each array in the _host_ buffer list, and upload data
	// if it is not in the skip list
	BufferList::iterator onhost = gdata->s_hBuffers.begin();
	const BufferList::iterator stop = gdata->s_hBuffers.end();
	for ( ; onhost != stop ; ++onhost) {
		flag_t buf_to_up = onhost->first;
		if (buf_to_up & skip_bufs)
			continue;

		AbstractBuffer *buf = m_dBuffers[buf_to_up];
		size_t _size = howManyParticles * buf->get_element_size();

		printf("Thread %d uploading %d %s items (%s) on device %d from position %d\n",
				m_deviceIndex, howManyParticles, buf->get_buffer_name(),
				gdata->memString(_size).c_str(), m_cudaDeviceNumber, firstInnerParticle);

		void *dstptr = buf->get_buffer(gdata->currentRead[buf_to_up]);
		const void *srcptr = onhost->second->get_buffer();
		CUDA_SAFE_CALL(cudaMemcpy(dstptr, srcptr, _size, cudaMemcpyHostToDevice));
	}
}

// Download the subset of the specified buffer to the correspondent shared CPU array.
// Makes multiple transfers. Only downloads the subset relative to the internal particles.
// For double buffered arrays, uses the READ buffers unless otherwise specified. Can be
// used for either the read or the write buffers, not both.
void GPUWorker::dumpBuffers() {
	// indices
	uint firstInnerParticle	= gdata->s_hStartPerDevice[m_deviceIndex];
	uint howManyParticles	= gdata->s_hPartsPerDevice[m_deviceIndex];

	// is the device empty? (unlikely but possible before LB kicks in)
	if (howManyParticles == 0) return;

	size_t _size = 0;
	flag_t flags = gdata->commandFlags;

	// iterate over each array in the _host_ buffer list, and download data
	// if it was requested
	BufferList::iterator onhost = gdata->s_hBuffers.begin();
	const BufferList::iterator stop = gdata->s_hBuffers.end();
	for ( ; onhost != stop ; ++onhost) {
		flag_t buf_to_get = onhost->first;
		if (!(buf_to_get & flags))
			continue;

		const AbstractBuffer *buf = m_dBuffers[buf_to_get];
		size_t _size = howManyParticles * buf->get_element_size();

		uint which_buffer = 0;
		if (flags & DBLBUFFER_READ) which_buffer = gdata->currentRead[buf_to_get];
		if (flags & DBLBUFFER_WRITE) which_buffer = gdata->currentWrite[buf_to_get];

		const void *srcptr = buf->get_buffer(which_buffer);
		void *dstptr = onhost->second->get_offset_buffer(0, firstInnerParticle);
		CUDA_SAFE_CALL(cudaMemcpy(dstptr, srcptr, _size, cudaMemcpyDeviceToHost));
	}
}

// Sets all cells as empty in device memory. Used before reorder
void GPUWorker::setDeviceCellsAsEmpty()
{
	CUDA_SAFE_CALL(cudaMemset(m_dCellStart, 0xffffffff, gdata->nGridCells  * sizeof(uint)));
}

// download cellStart and cellEnd to the shared arrays
void GPUWorker::downloadCellsIndices()
{
	size_t _size = gdata->nGridCells * sizeof(uint);
	CUDA_SAFE_CALL(cudaMemcpy(	gdata->s_dCellStarts[m_deviceIndex],
								m_dCellStart,
								_size, cudaMemcpyDeviceToHost));
	CUDA_SAFE_CALL(cudaMemcpy(	gdata->s_dCellEnds[m_deviceIndex],
								m_dCellEnd,
								_size, cudaMemcpyDeviceToHost));
	/*_size = 4 * sizeof(uint);
	CUDA_SAFE_CALL(cudaMemcpy(	gdata->s_dSegmentsStart[m_deviceIndex],
								m_dSegmentStart,
								_size, cudaMemcpyDeviceToHost));*/
}

void GPUWorker::downloadSegments()
{
	size_t _size = 4 * sizeof(uint);
	CUDA_SAFE_CALL(cudaMemcpy(	gdata->s_dSegmentsStart[m_deviceIndex],
								m_dSegmentStart,
								_size, cudaMemcpyDeviceToHost));
	/* printf("  T%d downloaded segs: (I) %u (IE) %u (OE) %u (O) %u\n", m_deviceIndex,
			gdata->s_dSegmentsStart[m_deviceIndex][0], gdata->s_dSegmentsStart[m_deviceIndex][1],
			gdata->s_dSegmentsStart[m_deviceIndex][2], gdata->s_dSegmentsStart[m_deviceIndex][3]); */
}

void GPUWorker::uploadSegments()
{
	size_t _size = 4 * sizeof(uint);
	CUDA_SAFE_CALL(cudaMemcpy(	m_dSegmentStart,
								gdata->s_dSegmentsStart[m_deviceIndex],
								_size, cudaMemcpyHostToDevice));
}

// download segments and update the number of internal particles
void GPUWorker::updateSegments()
{
	// if the device is empty, set the host and device segments as empty
	if (m_numParticles == 0)
		resetSegments();
	else {
		downloadSegments();
		// update the number of internal particles
		uint newNumIntParts = m_numParticles;

		if (gdata->s_dSegmentsStart[m_deviceIndex][CELLTYPE_OUTER_CELL] != EMPTY_SEGMENT)
			newNumIntParts = gdata->s_dSegmentsStart[m_deviceIndex][CELLTYPE_OUTER_CELL];

		if (gdata->s_dSegmentsStart[m_deviceIndex][CELLTYPE_OUTER_EDGE_CELL] != EMPTY_SEGMENT)
			newNumIntParts = gdata->s_dSegmentsStart[m_deviceIndex][CELLTYPE_OUTER_EDGE_CELL];

		m_particleRangeEnd = m_numInternalParticles = newNumIntParts;
	}
}

// set all segments, host and device, as empty
void GPUWorker::resetSegments()
{
	for (uint s = 0; s < 4; s++)
		gdata->s_dSegmentsStart[m_deviceIndex][s] = EMPTY_SEGMENT;
	uploadSegments();
}

// upload mbData for moving boundaries (possibily called many times)
void GPUWorker::uploadMBData()
{
	// check if MB are active and if gdata->s_mbData is not NULL
	if (m_simparams->mbcallback && gdata->s_mbData)
		setmbdata(gdata->s_mbData, gdata->mbDataSize);
}

// upload gravity (possibily called many times)
void GPUWorker::uploadGravity()
{
	// check if variable gravity is enabled
	if (m_simparams->gcallback)
		setgravity(gdata->s_varGravity);
}

// upload planes (called once until planes arae constant)
void GPUWorker::uploadPlanes()
{
	// check if planes > 0 (already checked before calling?)
	if (gdata->numPlanes > 0)
		setplaneconstants(gdata->numPlanes, gdata->s_hPlanesDiv, gdata->s_hPlanes);
}


// Create a compact device map, for this device, from the global one,
// with each cell being marked in the high bits. Correctly handles periodicity.
// Also handles the optional extra displacement for periodicity. Since the cell
// offset is truncated, we need to add more cells to the outer neighbors (the extra
// disp. vector might not be a multiple of the cell size). However, only one extra cell
// per cell is added. This means that we might miss cells if the extra displacement is
// not parallel to one cartesian axis.
void GPUWorker::createCompactDeviceMap() {
	// Here we have several possibilities:
	// 1. dynamic programming - visit each cell and half of its neighbors once, only write self
	//    (14*cells reads, 1*cells writes)
	// 2. visit each cell and all its neighbors, then write the output only for self
	//    (27*cells reads, 1*cells writes)
	// 3. same as 2 but reuse last read buffer; in the general case, it is possible to reuse 18 cells each time
	//    (9*cells reads, 1*cells writes)
	// 4. visit each cell; if it is assigned to self, visit its neighbors; set self and edging neighbors; if it is not self, write nothing and do not visit neighbors
	//    (initial memset, 1*extCells + 27*intCells reads, 27*intCells writes; if there are 2 devices per process, this means roughly ~14 reads and ~writes per cell)
	// 5. same as 4. but only make writes for alternate cells (3D chessboard: only white cells write on neibs)
	// 6. upload on the device, maybe recycling an existing buffer, and perform a parallel algorithm on the GPU
	//    (27xcells cached reads, cells writes)
	// 7. ...
	// Algorithms 1. and 3. may be the fastest. Number 2 is currently implemented; later will test 3.
	// One minor optimization has been implemented: iterating on neighbors stops as soon as there are enough information (e.g. cell belongs to self and there is
	// at least one neib which does not ==> inner_edge). Another optimization would be to avoid linearizing all cells but exploit burst of consecutive indices. This
	// reduces the computations but not the read/write operations.

	// compute the extra_offset vector (z-lift) in terms of cells
	int3 extra_offset = make_int3(0);
	// Direction of the extra_offset (depents on where periodicity occurs), to add extra cells if the extra displacement
	// does not multiply the size of the cells
	int3 extra_offset_unit_vector = make_int3(0);
	if (m_simparams->periodicbound) {
		extra_offset.x = (int) (m_physparams->dispOffset.x / gdata->cellSize.x);
		extra_offset.y = (int) (m_physparams->dispOffset.y / gdata->cellSize.y);
		extra_offset.z = (int) (m_physparams->dispOffset.z / gdata->cellSize.z);
	}

	// when there is an extra_offset to consider, we have to repeat the same checks twice. Let's group them
#define CHECK_CURRENT_CELL \
	/* data of neib cell */ \
	uint neib_lin_idx = gdata->calcGridHashHost(cx, cy, cz); \
	uint neib_globalDevidx = gdata->s_hDeviceMap[neib_lin_idx]; \
	any_mine_neib	 |= (neib_globalDevidx == m_globalDeviceIdx); \
	any_foreign_neib |= (neib_globalDevidx != m_globalDeviceIdx); \
	/* did we read enough to decide for current cell? */ \
	enough_info = (is_mine && any_foreign_neib) || (!is_mine && any_mine_neib);

	// iterate on all cells of the world
	for (int ix=0; ix < gdata->gridSize.x; ix++)
		for (int iy=0; iy < gdata->gridSize.y; iy++)
			for (int iz=0; iz < gdata->gridSize.z; iz++) {
				// data of current cell
				uint cell_lin_idx = gdata->calcGridHashHost(ix, iy, iz);
				uint cell_globalDevidx = gdata->s_hDeviceMap[cell_lin_idx];
				bool is_mine = (cell_globalDevidx == m_globalDeviceIdx);
				// aux vars for iterating on neibs
				bool any_foreign_neib = false; // at least one neib does not belong to me?
				bool any_mine_neib = false; // at least one neib does belong to me?
				bool enough_info = false; // when true, stop iterating on neibs
				// iterate on neighbors
				for (int dx=-1; dx <= 1 && !enough_info; dx++)
					for (int dy=-1; dy <= 1 && !enough_info; dy++)
						for (int dz=-1; dz <= 1 && !enough_info; dz++) {
							// do not iterate on self
							if (dx == 0 && dy == 0 && dz == 0) continue;
							// explicit cell coordinates for readability
							int cx = ix + dx;
							int cy = iy + dy;
							int cz = iz + dz;
							bool apply_extra_offset = false;
							// warp periodic boundaries
							if (m_simparams->periodicbound) {
								// periodicity along X
								if (m_physparams->dispvect.x) {
									// WARNING: checking if c* is negative MUST be done before checking if it's greater than
									// the grid, otherwise it will be cast to uint and "-1" will be "greater" than the gridSize
									if (cx < 0) {
										cx = gdata->gridSize.x - 1;
										if (extra_offset.y != 0 || extra_offset.z != 0) {
											extra_offset_unit_vector.y = extra_offset_unit_vector.z = 1;
											apply_extra_offset = true;
										}
									} else
									if (cx >= gdata->gridSize.x) {
										cx = 0;
										if (extra_offset.y != 0 || extra_offset.z != 0) {
											extra_offset_unit_vector.y = extra_offset_unit_vector.z = -1;
											apply_extra_offset = true;
										}
									}
								} // if dispvect.x
								// periodicity along Y
								if (m_physparams->dispvect.y) {
									if (cy < 0) {
										cy = gdata->gridSize.y - 1;
										if (extra_offset.x != 0 || extra_offset.z != 0) {
											extra_offset_unit_vector.x = extra_offset_unit_vector.z = 1;
											apply_extra_offset = true;
										}
									} else
									if (cy >= gdata->gridSize.y) {
										cy = 0;
										if (extra_offset.x != 0 || extra_offset.z != 0) {
											extra_offset_unit_vector.x = extra_offset_unit_vector.z = -1;
											apply_extra_offset = true;
										}
									}
								} // if dispvect.y
								// periodicity along Z
								if (m_physparams->dispvect.z) {
									if (cz < 0) {
										cz = gdata->gridSize.z - 1;
										if (extra_offset.x != 0 || extra_offset.y != 0) {
											extra_offset_unit_vector.x = extra_offset_unit_vector.y = 1;
											apply_extra_offset = true;
										}
									} else
									if (cz >= gdata->gridSize.z) {
										cz = 0;
										if (extra_offset.x != 0 || extra_offset.y != 0) {
											extra_offset_unit_vector.x = extra_offset_unit_vector.y = -1;
											apply_extra_offset = true;
										}
									}
								} // if dispvect.z
								// apply extra displacement (e.g. zlift), if any
								// (actually, should be safe to add without the conditional)
								if (apply_extra_offset) {
									cx += extra_offset.x * extra_offset_unit_vector.x;
									cy += extra_offset.y * extra_offset_unit_vector.y;
									cz += extra_offset.z * extra_offset_unit_vector.z;
								}
							}
							// if not periodic, or if still out-of-bounds after periodicity warp, skip it
							if (cx < 0 || cx >= gdata->gridSize.x ||
								cy < 0 || cy >= gdata->gridSize.y ||
								cz < 0 || cz >= gdata->gridSize.z) continue;
							// check current cell and if we have already enough information
							CHECK_CURRENT_CELL
							// if there is any extra offset, check one more cell
							// WARNING: might miss cells if the extra displacement is not parallel to one cartesian axis
							if (m_simparams->periodicbound && apply_extra_offset) {
								cx += extra_offset_unit_vector.x;
								cy += extra_offset_unit_vector.y;
								cz += extra_offset_unit_vector.z;
								// check extra cells if extra displacement is not a multiple of the cell size
								CHECK_CURRENT_CELL
							}
						} // iterating on offsets of neighbor cells
				uint cellType;
				// assign shifted values so that they are ready to be OR'd in calchash/reorder
				if (is_mine && !any_foreign_neib)	cellType = CELLTYPE_INNER_CELL_SHIFTED;
				if (is_mine && any_foreign_neib)	cellType = CELLTYPE_INNER_EDGE_CELL_SHIFTED;
				if (!is_mine && any_mine_neib)		cellType = CELLTYPE_OUTER_EDGE_CELL_SHIFTED;
				if (!is_mine && !any_mine_neib)		cellType = CELLTYPE_OUTER_CELL_SHIFTED;
				m_hCompactDeviceMap[cell_lin_idx] = cellType;
			}
	// here it is possible to save the compact device map
	// gdata->saveCompactDeviceMapToFile("", m_deviceIndex, m_hCompactDeviceMap);
}

// self-explanatory
void GPUWorker::uploadCompactDeviceMap() {
	size_t _size = m_nGridCells * sizeof(uint);
	CUDA_SAFE_CALL(cudaMemcpy(m_dCompactDeviceMap, m_hCompactDeviceMap, _size, cudaMemcpyHostToDevice));
}

// this should be singleton, i.e. should check that no other thread has been started (mutex + counter or bool)
void GPUWorker::run_worker() {
	// wrapper for pthread_create()
	// NOTE: the dynamic instance of the GPUWorker is passed as parameter
	pthread_create(&pthread_id, NULL, simulationThread, (void*)this);
}

// Join the simulation thread (in pthreads' terminology)
// WARNING: blocks the caller until the thread reaches pthread_exit. Be sure to call it after all barriers
// have been reached or may result in deadlock!
void GPUWorker::join_worker() {
	pthread_join(pthread_id, NULL);
}

GlobalData* GPUWorker::getGlobalData() {
	return gdata;
}

unsigned int GPUWorker::getCUDADeviceNumber()
{
	return m_cudaDeviceNumber;
}

unsigned int GPUWorker::getDeviceIndex()
{
	return m_deviceIndex;
}

cudaDeviceProp GPUWorker::getDeviceProperties() {
	return m_deviceProperties;
}

size_t GPUWorker::getHostMemory() {
	return m_hostMemory;
}

size_t GPUWorker::getDeviceMemory() {
	return m_deviceMemory;
}

const AbstractBuffer* GPUWorker::getBuffer(flag_t key) const
{
	return m_dBuffers[key];
}

void GPUWorker::setDeviceProperties(cudaDeviceProp _m_deviceProperties) {
	m_deviceProperties = _m_deviceProperties;
}

// enable direct p2p memory transfers by allowing the other devices to access the current device memory
void GPUWorker::enablePeerAccess()
{
	// iterate on all devices
	for (uint d=0; d < gdata->devices; d++) {
		// skip self
		if (d == m_deviceIndex) continue;
		// is peer access possible?
		int res;
		cudaDeviceCanAccessPeer(&res, m_deviceIndex, d);
		if (res != 1)
			printf("WARNING: device %u cannot enable peer access of device %u; peer copies will be buffered on host\n", m_deviceIndex, d);
		else
			cudaDeviceEnablePeerAccess(d, 0);
	}
}

// Actual thread calling GPU-methods
void* GPUWorker::simulationThread(void *ptr) {
	// INITIALIZATION PHASE

	// take the pointer of the instance starting this thread
	GPUWorker* instance = (GPUWorker*) ptr;

	// retrieve GlobalData and device number (index in process array)
	const GlobalData* gdata = instance->getGlobalData();
	const unsigned int cudaDeviceNumber = instance->getCUDADeviceNumber();
	const unsigned int deviceIndex = instance->getDeviceIndex();

	instance->setDeviceProperties( checkCUDA(gdata, deviceIndex) );

	// allow peers to access the device memory (for cudaMemcpyPeer[Async])
	instance->enablePeerAccess();

	// compute #parts to allocate according to the free memory on the device
	// must be done before uploading constants since some constants
	// (e.g. those for neibslist traversal) depend on the number of particles
	// allocated
	instance->computeAndSetAllocableParticles();

	// upload constants (PhysParames, some SimParams)
	instance->uploadConstants();

	// upload planes, if any
	instance->uploadPlanes();

		// upload centers of gravity of the bodies
	instance->uploadBodiesCentersOfGravity();

	// allocate CPU and GPU arrays
	instance->allocateHostBuffers();
	instance->allocateDeviceBuffers();
	instance->printAllocatedMemory();

	// create and upload the compact device map (2 bits per cell)
	if (MULTI_DEVICE) {
		instance->createCompactDeviceMap();
		instance->uploadCompactDeviceMap();
	}

	// TODO: here set_reduction_params() will be called (to be implemented in this class). These parameters can be device-specific.

	// TODO: here setDemTexture() will be called. It is device-wide, but reading the DEM file is process wide and will be in GPUSPH class

	// init streams for async memcpys (only useful for multigpu?)
	instance->createStreams();

	gdata->threadSynchronizer->barrier(); // end of INITIALIZATION ***

	// here GPUSPH::initialize is over and GPUSPH::runSimulation() is called

	gdata->threadSynchronizer->barrier(); // begins UPLOAD ***

	instance->uploadSubdomain();

	gdata->threadSynchronizer->barrier();  // end of UPLOAD, begins SIMULATION ***

	// TODO
	// Here is a copy-paste from the CPU thread worker of branch cpusph, as a canvas
	while (gdata->keep_going) {
		switch (gdata->nextCommand) {
			// logging here?
			case IDLE:
				break;
			case CALCHASH:
				//printf(" T %d issuing HASH\n", deviceIndex);
				instance->kernel_calcHash();
				break;
			case SORT:
				//printf(" T %d issuing SORT\n", deviceIndex);
				instance->kernel_sort();
				break;
			case CROP:
				//printf(" T %d issuing CROP\n", deviceIndex);
				instance->dropExternalParticles();
				break;
			case REORDER:
				//printf(" T %d issuing REORDER\n", deviceIndex);
				instance->kernel_reorderDataAndFindCellStart();
				break;
			case BUILDNEIBS:
				//printf(" T %d issuing BUILDNEIBS\n", deviceIndex);
				instance->kernel_buildNeibsList();
				break;
			case FORCES:
				//printf(" T %d issuing FORCES\n", deviceIndex);
				instance->kernel_forces();
				break;
			case EULER:
				//printf(" T %d issuing EULER\n", deviceIndex);
				instance->kernel_euler();
				break;
			case DUMP:
				//printf(" T %d issuing DUMP\n", deviceIndex);
				instance->dumpBuffers();
				break;
			case DUMP_CELLS:
				//printf(" T %d issuing DUMP_CELLS\n", deviceIndex);
				instance->downloadCellsIndices();
				break;
			case UPDATE_SEGMENTS:
				//printf(" T %d issuing UPDATE_SEGMENTS\n", deviceIndex);
				instance->updateSegments();
				break;
			case APPEND_EXTERNAL:
				//printf(" T %d issuing APPEND_EXTERNAL\n", deviceIndex);
				if (MULTI_GPU)
					instance->importPeerEdgeCells();
				if (MULTI_NODE)
					instance->importNetworkPeerEdgeCells();
				break;
			case UPDATE_EXTERNAL:
				//printf(" T %d issuing UPDATE_EXTERNAL\n", deviceIndex);
				if (MULTI_GPU)
					instance->importPeerEdgeCells();
				if (MULTI_NODE)
					instance->importNetworkPeerEdgeCells();
				break;
			case MLS:
				//printf(" T %d issuing MLS\n", deviceIndex);
				instance->kernel_mls();
				break;
			case SHEPARD:
				//printf(" T %d issuing SHEPARD\n", deviceIndex);
				instance->kernel_shepard();
				break;
			case VORTICITY:
				//printf(" T %d issuing VORTICITY\n", deviceIndex);
				instance->kernel_vorticity();
				break;
			case SURFACE_PARTICLES:
				//printf(" T %d issuing SURFACE_PARTICLES\n", deviceIndex);
				instance->kernel_surfaceParticles();
				break;
			case MF_INIT_GAMMA:
				//printf(" T %d issuing MF_INIT_GAMMA\n", deviceIndex);
				instance->kernel_initGradGamma();
				break;
			case MF_UPDATE_GAMMA:
				//printf(" T %d issuing MF_UPDATE_GAMMA\n", deviceIndex);
				instance->kernel_updateGamma();
				break;
			case MF_UPDATE_POS:
				//printf(" T %d issuing MF_UPDATE_POS\n", deviceIndex);
				instance->kernel_updatePositions();
				break;
			case MF_CALC_BOUND_CONDITIONS:
				//printf(" T %d issuing MF_CALC_BOUND_CONDITIONS\n", deviceIndex);
				instance->kernel_dynamicBoundaryConditions();
				break;
			case MF_UPDATE_BOUND_VALUES:
				//printf(" T %d issuing MF_UPDATE_BOUND_VALUES\n", deviceIndex);
				instance->kernel_updateValuesAtBoundaryElements();
				break;
			case SPS:
				//printf(" T %d issuing SPS\n", deviceIndex);
				instance->kernel_sps();
				break;
			case MEAN_STRAIN:
				//printf(" T %d issuing MEAN_STRAIN\n", deviceIndex);
				instance->kernel_meanStrain();
			case REDUCE_BODIES_FORCES:
				//printf(" T %d issuing REDUCE_BODIES_FORCES\n", deviceIndex);
				instance->kernel_reduceRBForces();
				break;
			case UPLOAD_MBDATA:
				//printf(" T %d issuing UPLOAD_MBDATA\n", deviceIndex);
				instance->uploadMBData();
				break;
			case UPLOAD_GRAVITY:
				//printf(" T %d issuing UPLOAD_GRAVITY\n", deviceIndex);
				instance->uploadGravity();
				break;
			case UPLOAD_PLANES:
				//printf(" T %d issuing UPLOAD_PLANES\n", deviceIndex);
				instance->uploadPlanes();
				break;
			case UPLOAD_OBJECTS_CG:
				//printf(" T %d issuing UPLOAD_OBJECTS_CG\n", deviceIndex);
				instance->uploadBodiesCentersOfGravity();
				break;
			case UPLOAD_OBJECTS_MATRICES:
				//printf(" T %d issuing UPLOAD_OBJECTS_CG\n", deviceIndex);
				instance->uploadBodiesTransRotMatrices();
				break;
			case QUIT:
				//printf(" T %d issuing QUIT\n", deviceIndex);
				// actually, setting keep_going to false and unlocking the barrier should be enough to quit the cycle
				break;
		}
		if (gdata->keep_going) {
			// the first barrier waits for the main thread to set the next command; the second is to unlock
			gdata->threadSynchronizer->barrier();  // CYCLE BARRIER 1
			gdata->threadSynchronizer->barrier();  // CYCLE BARRIER 2
		}
	}

	gdata->threadSynchronizer->barrier();  // end of SIMULATION, begins FINALIZATION ***

	// destroy streams
	instance->destroyStreams();

	// deallocate buffers
	instance->deallocateHostBuffers();
	instance->deallocateDeviceBuffers();
	// ...what else?

	gdata->threadSynchronizer->barrier();  // end of FINALIZATION ***

	pthread_exit(NULL);
}

void GPUWorker::kernel_calcHash()
{
	// is the device empty? (unlikely but possible before LB kicks in)
	if (m_numParticles == 0) return;

	calcHash(	m_dBuffers.getBufferData<BUFFER_POS>(gdata->currentRead[BUFFER_POS]),
				m_dBuffers.getBufferData<BUFFER_HASH>(),
				m_dBuffers.getBufferData<BUFFER_PARTINDEX>(),
				m_dBuffers.getBufferData<BUFFER_INFO>(gdata->currentRead[BUFFER_INFO]),
#if HASH_KEY_SIZE >= 64
				m_dCompactDeviceMap,
#endif
				m_numParticles,
				m_simparams->periodicbound);
}

void GPUWorker::kernel_sort()
{
	uint numPartsToElaborate = (gdata->only_internal ? m_numInternalParticles : m_numParticles);

	// is the device empty? (unlikely but possible before LB kicks in)
	if (numPartsToElaborate == 0) return;

	sort(	m_dBuffers.getBufferData<BUFFER_HASH>(),
			m_dBuffers.getBufferData<BUFFER_PARTINDEX>(),
			numPartsToElaborate);
}

void GPUWorker::kernel_reorderDataAndFindCellStart()
{
	// reset also if the device is empty (or we will download uninitialized values)
	setDeviceCellsAsEmpty();

	// is the device empty? (unlikely but possible before LB kicks in)
	if (m_numParticles == 0) return;

	// TODO this kernel needs a thorough reworking to only pass the needed buffers
	reorderDataAndFindCellStart(m_dCellStart,	  // output: cell start index
							m_dCellEnd,		// output: cell end index
#if HASH_KEY_SIZE >= 64
							m_dSegmentStart,
#endif
							// output: sorted arrays
							m_dBuffers.getBufferData<BUFFER_POS>(gdata->currentWrite[BUFFER_POS]),
							m_dBuffers.getBufferData<BUFFER_VEL>(gdata->currentWrite[BUFFER_VEL]),
							m_dBuffers.getBufferData<BUFFER_INFO>(gdata->currentWrite[BUFFER_INFO]),
							m_dBuffers.getBufferData<BUFFER_BOUNDELEMENTS>(gdata->currentWrite[BUFFER_BOUNDELEMENTS]),
							m_dBuffers.getBufferData<BUFFER_GRADGAMMA>(gdata->currentWrite[BUFFER_GRADGAMMA]),
							m_dBuffers.getBufferData<BUFFER_VERTICES>(gdata->currentWrite[BUFFER_VERTICES]),
							m_dBuffers.getBufferData<BUFFER_PRESSURE>(gdata->currentWrite[BUFFER_PRESSURE]),
							m_dBuffers.getBufferData<BUFFER_TKE>(gdata->currentWrite[BUFFER_TKE]),
							m_dBuffers.getBufferData<BUFFER_EPSILON>(gdata->currentWrite[BUFFER_EPSILON]),
							m_dBuffers.getBufferData<BUFFER_TURBVISC>(gdata->currentWrite[BUFFER_TURBVISC]),
							m_dBuffers.getBufferData<BUFFER_STRAIN_RATE>(gdata->currentWrite[BUFFER_STRAIN_RATE]),

							// hash
							m_dBuffers.getBufferData<BUFFER_HASH>(),
							// sorted particle indices
							m_dBuffers.getBufferData<BUFFER_PARTINDEX>(),

							// input: arrays to sort
							m_dBuffers.getBufferData<BUFFER_POS>(gdata->currentRead[BUFFER_POS]),
							m_dBuffers.getBufferData<BUFFER_VEL>(gdata->currentRead[BUFFER_VEL]),
							m_dBuffers.getBufferData<BUFFER_INFO>(gdata->currentRead[BUFFER_INFO]),
							m_dBuffers.getBufferData<BUFFER_BOUNDELEMENTS>(gdata->currentRead[BUFFER_BOUNDELEMENTS]),
							m_dBuffers.getBufferData<BUFFER_GRADGAMMA>(gdata->currentRead[BUFFER_GRADGAMMA]),
							m_dBuffers.getBufferData<BUFFER_VERTICES>(gdata->currentRead[BUFFER_VERTICES]),
							m_dBuffers.getBufferData<BUFFER_PRESSURE>(gdata->currentRead[BUFFER_PRESSURE]),
							m_dBuffers.getBufferData<BUFFER_TKE>(gdata->currentRead[BUFFER_TKE]),
							m_dBuffers.getBufferData<BUFFER_EPSILON>(gdata->currentRead[BUFFER_EPSILON]),
							m_dBuffers.getBufferData<BUFFER_TURBVISC>(gdata->currentRead[BUFFER_TURBVISC]),
							m_dBuffers.getBufferData<BUFFER_STRAIN_RATE>(gdata->currentRead[BUFFER_STRAIN_RATE]),

							m_numParticles,
							m_nGridCells,
							m_dBuffers.getBufferData<BUFFER_INVINDEX>());
}

void GPUWorker::kernel_buildNeibsList()
{
	uint numPartsToElaborate = (gdata->only_internal ? m_particleRangeEnd : m_numParticles);

	// is the device empty? (unlikely but possible before LB kicks in)
	if (numPartsToElaborate == 0) return;

	buildNeibsList(	m_dBuffers.getBufferData<BUFFER_NEIBSLIST>(),
					m_dBuffers.getBufferData<BUFFER_POS>(gdata->currentRead[BUFFER_POS]),
					m_dBuffers.getBufferData<BUFFER_INFO>(gdata->currentRead[BUFFER_INFO]),
					m_dBuffers.getBufferData<BUFFER_HASH>(),
					m_dCellStart,
					m_dCellEnd,
					m_numParticles,
					numPartsToElaborate,
					m_nGridCells,
					m_simparams->nlSqInfluenceRadius,
					m_simparams->periodicbound);
}

void GPUWorker::kernel_forces()
{
	uint numPartsToElaborate = (gdata->only_internal ? m_particleRangeEnd : m_numParticles);

	// FLOAT_MAX is returned if kernels are not run (e.g. numPartsToElaborate == 0)
	float returned_dt = FLT_MAX;

	bool firstStep = (gdata->commandFlags == INTEGRATOR_STEP_1);

	// if we have objects potentially shared across different devices, must reset their forces
	// and torques to avoid spurious contributions
	if (m_simparams->numODEbodies > 0 && MULTI_DEVICE) {
		uint bodiesPartsSize = m_numBodiesParticles * sizeof(float4);
		CUDA_SAFE_CALL(cudaMemset(m_dRbForces, 0.0F, bodiesPartsSize));
		CUDA_SAFE_CALL(cudaMemset(m_dRbTorques, 0.0F, bodiesPartsSize));
	}

	// first step
	if (numPartsToElaborate > 0 && firstStep)
		returned_dt = forces(
						m_dBuffers.getBufferData<BUFFER_POS>(gdata->currentRead[BUFFER_POS]),   // pos(n)
						m_dBuffers.getBufferData<BUFFER_VEL>(gdata->currentRead[BUFFER_VEL]),   // vel(n)
						m_dBuffers.getBufferData<BUFFER_FORCES>(),					// f(n
						m_dBuffers.getBufferData<BUFFER_GRADGAMMA>(gdata->currentRead[BUFFER_GRADGAMMA]),
						m_dBuffers.getBufferData<BUFFER_BOUNDELEMENTS>(gdata->currentRead[BUFFER_BOUNDELEMENTS]),
						m_dBuffers.getBufferData<BUFFER_PRESSURE>(gdata->currentRead[BUFFER_PRESSURE]),
						m_dRbForces,
						m_dRbTorques,
						m_dBuffers.getBufferData<BUFFER_XSPH>(),
						m_dBuffers.getBufferData<BUFFER_INFO>(gdata->currentRead[BUFFER_INFO]),
						m_dBuffers.getBufferData<BUFFER_HASH>(),
						m_dCellStart,
						m_dBuffers.getBufferData<BUFFER_NEIBSLIST>(),
						m_numParticles,
						numPartsToElaborate,
						gdata->problem->m_deltap,
						m_simparams->slength,
						gdata->dt, // m_dt,
						m_simparams->dtadapt,
						m_simparams->dtadaptfactor,
						m_simparams->xsph,
						m_simparams->kerneltype,
						m_simparams->influenceRadius,
						m_simparams->visctype,
						m_physparams->visccoeff,
						m_dBuffers.getBufferData<BUFFER_TURBVISC>(gdata->currentRead[BUFFER_TURBVISC]),	// nu_t(n)
						m_dBuffers.getBufferData<BUFFER_TKE>(gdata->currentRead[BUFFER_TKE]),	// k(n)
						m_dBuffers.getBufferData<BUFFER_EPSILON>(gdata->currentRead[BUFFER_EPSILON]),	// e(n)
						m_dBuffers.getBufferData<BUFFER_DKDE>(),
						m_dBuffers.getBufferData<BUFFER_CFL>(),
						m_dBuffers.getBufferData<BUFFER_CFL_GAMMA>(),
						m_dBuffers.getBufferData<BUFFER_CFL_KEPS>(),
						m_dBuffers.getBufferData<BUFFER_CFL_TEMP>(),
						m_simparams->sph_formulation,
						m_simparams->boundarytype,
						m_simparams->usedem);
	else
	// second step
	if (numPartsToElaborate > 0 && !firstStep)
		returned_dt = forces(  m_dBuffers.getBufferData<BUFFER_POS>(gdata->currentWrite[BUFFER_POS]),  // pos(n+1/2)
						m_dBuffers.getBufferData<BUFFER_VEL>(gdata->currentWrite[BUFFER_VEL]),  // vel(n+1/2)
						m_dBuffers.getBufferData<BUFFER_FORCES>(),					// f(n+1/2)
						m_dBuffers.getBufferData<BUFFER_GRADGAMMA>(gdata->currentRead[BUFFER_GRADGAMMA]),
						m_dBuffers.getBufferData<BUFFER_BOUNDELEMENTS>(gdata->currentRead[BUFFER_BOUNDELEMENTS]),
						m_dBuffers.getBufferData<BUFFER_PRESSURE>(gdata->currentRead[BUFFER_PRESSURE]),
						m_dRbForces,
						m_dRbTorques,
						m_dBuffers.getBufferData<BUFFER_XSPH>(),
						m_dBuffers.getBufferData<BUFFER_INFO>(gdata->currentRead[BUFFER_INFO]),
						m_dBuffers.getBufferData<BUFFER_HASH>(),
						m_dCellStart,
						m_dBuffers.getBufferData<BUFFER_NEIBSLIST>(),
						m_numParticles,
						numPartsToElaborate,
						gdata->problem->m_deltap,
						m_simparams->slength,
						gdata->dt, // m_dt,
						m_simparams->dtadapt,
						m_simparams->dtadaptfactor,
						m_simparams->xsph,
						m_simparams->kerneltype,
						m_simparams->influenceRadius,
						m_simparams->visctype,
						m_physparams->visccoeff,
						m_dBuffers.getBufferData<BUFFER_TURBVISC>(gdata->currentRead[BUFFER_TURBVISC]),	// nu_t(n+1/2)
						m_dBuffers.getBufferData<BUFFER_TKE>(gdata->currentWrite[BUFFER_TKE]),	// k(n+1/2)
						m_dBuffers.getBufferData<BUFFER_EPSILON>(gdata->currentWrite[BUFFER_EPSILON]),	// e(n+1/2)
						m_dBuffers.getBufferData<BUFFER_DKDE>(),
						m_dBuffers.getBufferData<BUFFER_CFL>(),
						m_dBuffers.getBufferData<BUFFER_CFL_GAMMA>(),
						m_dBuffers.getBufferData<BUFFER_CFL_KEPS>(),
						m_dBuffers.getBufferData<BUFFER_CFL_TEMP>(),
						m_simparams->sph_formulation,
						m_simparams->boundarytype,
						m_simparams->usedem );

	// gdata->dts is directly used instead of handling dt1 and dt2
	//printf(" Step %d, bool %d, returned %g, current %g, ",
	//	gdata->step, firstStep, returned_dt, gdata->dts[devnum]);
	if (firstStep)
		gdata->dts[m_deviceIndex] = returned_dt;
	else
		gdata->dts[m_deviceIndex] = min(gdata->dts[m_deviceIndex], returned_dt);
	//printf("set to %g\n",gdata->dts[m_deviceIndex]);
}

void GPUWorker::kernel_euler()
{
	uint numPartsToElaborate = (gdata->only_internal ? m_particleRangeEnd : m_numParticles);

	// is the device empty? (unlikely but possible before LB kicks in)
	if (numPartsToElaborate == 0) return;

	bool firstStep = (gdata->commandFlags == INTEGRATOR_STEP_1);

		euler(
			// previous pos, vel, k, e, info
			m_dBuffers.getBufferData<BUFFER_POS>(gdata->currentRead[BUFFER_POS]),
			m_dBuffers.getBufferData<BUFFER_HASH>(),
			m_dBuffers.getBufferData<BUFFER_VEL>(gdata->currentRead[BUFFER_VEL]),
			m_dBuffers.getBufferData<BUFFER_TKE>(gdata->currentRead[BUFFER_TKE]),
			m_dBuffers.getBufferData<BUFFER_EPSILON>(gdata->currentRead[BUFFER_EPSILON]),
			m_dBuffers.getBufferData<BUFFER_INFO>(gdata->currentRead[BUFFER_INFO]),
			// f(n+1/2)
			m_dBuffers.getBufferData<BUFFER_FORCES>(),
			// dkde(n)
			m_dBuffers.getBufferData<BUFFER_DKDE>(),
			m_dBuffers.getBufferData<BUFFER_XSPH>(),
			// integrated pos vel, k, e
			m_dBuffers.getBufferData<BUFFER_POS>(gdata->currentWrite[BUFFER_POS]),
			m_dBuffers.getBufferData<BUFFER_VEL>(gdata->currentWrite[BUFFER_VEL]),
			m_dBuffers.getBufferData<BUFFER_TKE>(gdata->currentWrite[BUFFER_TKE]),
			m_dBuffers.getBufferData<BUFFER_EPSILON>(gdata->currentWrite[BUFFER_EPSILON]),
			m_numParticles,
			numPartsToElaborate,
			gdata->dt, // m_dt,
			gdata->dt/2.0f, // m_dt/2.0,
			firstStep ? 1 : 2,
			gdata->t + (firstStep ? gdata->dt / 2.0f : gdata->dt),
			m_simparams->xsph);
}

void GPUWorker::kernel_mls()
{
	uint numPartsToElaborate = (gdata->only_internal ? m_particleRangeEnd : m_numParticles);

	// is the device empty? (unlikely but possible before LB kicks in)
	if (numPartsToElaborate == 0) return;

	mls(m_dBuffers.getBufferData<BUFFER_POS>(gdata->currentRead[BUFFER_POS]),
		m_dBuffers.getBufferData<BUFFER_VEL>(gdata->currentRead[BUFFER_VEL]),
		m_dBuffers.getBufferData<BUFFER_VEL>(gdata->currentWrite[BUFFER_VEL]),
		m_dBuffers.getBufferData<BUFFER_INFO>(gdata->currentRead[BUFFER_INFO]),
		m_dBuffers.getBufferData<BUFFER_HASH>(),
		m_dCellStart,
		m_dBuffers.getBufferData<BUFFER_NEIBSLIST>(),
		m_numParticles,
		numPartsToElaborate,
		m_simparams->slength,
		m_simparams->kerneltype,
		m_simparams->influenceRadius);
}

void GPUWorker::kernel_shepard()
{
	uint numPartsToElaborate = (gdata->only_internal ? m_particleRangeEnd : m_numParticles);

	// is the device empty? (unlikely but possible before LB kicks in)
	if (numPartsToElaborate == 0) return;

	shepard(m_dBuffers.getBufferData<BUFFER_POS>(gdata->currentRead[BUFFER_POS]),
			m_dBuffers.getBufferData<BUFFER_VEL>(gdata->currentRead[BUFFER_VEL]),
			m_dBuffers.getBufferData<BUFFER_VEL>(gdata->currentWrite[BUFFER_VEL]),
			m_dBuffers.getBufferData<BUFFER_INFO>(gdata->currentRead[BUFFER_INFO]),
			m_dBuffers.getBufferData<BUFFER_HASH>(),
			m_dCellStart,
			m_dBuffers.getBufferData<BUFFER_NEIBSLIST>(),
			m_numParticles,
			numPartsToElaborate,
			m_simparams->slength,
			m_simparams->kerneltype,
			m_simparams->influenceRadius);
}

void GPUWorker::kernel_vorticity()
{
	uint numPartsToElaborate = (gdata->only_internal ? m_particleRangeEnd : m_numParticles);

	// is the device empty? (unlikely but possible before LB kicks in)
	if (numPartsToElaborate == 0) return;

	// Calling vorticity computation kernel
	vorticity(	m_dBuffers.getBufferData<BUFFER_POS>(gdata->currentRead[BUFFER_POS]),
				m_dBuffers.getBufferData<BUFFER_VEL>(gdata->currentRead[BUFFER_VEL]),
				m_dBuffers.getBufferData<BUFFER_VORTICITY>(),
				m_dBuffers.getBufferData<BUFFER_INFO>(gdata->currentRead[BUFFER_INFO]),
				m_dBuffers.getBufferData<BUFFER_HASH>(),
				m_dCellStart,
				m_dBuffers.getBufferData<BUFFER_NEIBSLIST>(),
				m_numParticles,
				numPartsToElaborate,
				m_simparams->slength,
				m_simparams->kerneltype,
				m_simparams->influenceRadius);
}

void GPUWorker::kernel_surfaceParticles()
{
	uint numPartsToElaborate = (gdata->only_internal ? m_particleRangeEnd : m_numParticles);

	// is the device empty? (unlikely but possible before LB kicks in)
	if (numPartsToElaborate == 0) return;

	surfaceparticle(m_dBuffers.getBufferData<BUFFER_POS>(gdata->currentRead[BUFFER_POS]),
					m_dBuffers.getBufferData<BUFFER_VEL>(gdata->currentRead[BUFFER_VEL]),
					m_dBuffers.getBufferData<BUFFER_NORMALS>(),
					m_dBuffers.getBufferData<BUFFER_INFO>(gdata->currentRead[BUFFER_INFO]),
					m_dBuffers.getBufferData<BUFFER_INFO>(gdata->currentWrite[BUFFER_INFO]),
					m_dBuffers.getBufferData<BUFFER_HASH>(),
					m_dCellStart,
					m_dBuffers.getBufferData<BUFFER_NEIBSLIST>(),
					m_numParticles,
					numPartsToElaborate,
					m_simparams->slength,
					m_simparams->kerneltype,
					m_simparams->influenceRadius,
					m_simparams->savenormals);
}

void GPUWorker::kernel_sps()
{
	uint numPartsToElaborate = (gdata->only_internal ? m_particleRangeEnd : m_numParticles);

	// is the device empty? (unlikely but possible before LB kicks in)
	if (numPartsToElaborate == 0) return;

	// pos and vel are read from curren*Read on the first step,
	// from current*Write on the second
	bool firstStep = (gdata->commandFlags & INTEGRATOR_STEP_1);
	uint posRead = firstStep ? gdata->currentRead[BUFFER_POS] : gdata->currentWrite[BUFFER_POS];
	uint velRead = firstStep ? gdata->currentRead[BUFFER_VEL] : gdata->currentWrite[BUFFER_VEL];

	sps(m_dBuffers.getBuffer<BUFFER_TAU>()->get_raw_ptr(),
		m_dBuffers.getBufferData<BUFFER_POS>(posRead),
		m_dBuffers.getBufferData<BUFFER_VEL>(velRead),
		m_dBuffers.getBufferData<BUFFER_INFO>(gdata->currentRead[BUFFER_INFO]),
		m_dBuffers.getBufferData<BUFFER_HASH>(),
		m_dCellStart,
		m_dBuffers.getBufferData<BUFFER_NEIBSLIST>(),
		m_numParticles,
		numPartsToElaborate,
		m_simparams->slength,
		m_simparams->kerneltype,
		m_simparams->influenceRadius);
}

void GPUWorker::kernel_meanStrain()
{
	uint numPartsToElaborate = (gdata->only_internal ? m_particleRangeEnd : m_numParticles);

	// is the device empty? (unlikely but possible before LB kicks in)
	if (numPartsToElaborate == 0) return;

	// pos and vel are read from curren*Read on the first step,
	// from current*Write on the second
	bool firstStep = (gdata->commandFlags & INTEGRATOR_STEP_1);
	uint posRead = firstStep ? gdata->currentRead[BUFFER_POS] : gdata->currentWrite[BUFFER_POS];
	uint velRead = firstStep ? gdata->currentRead[BUFFER_VEL] : gdata->currentWrite[BUFFER_VEL];

	mean_strain_rate(
		m_dBuffers.getBufferData<BUFFER_STRAIN_RATE>(gdata->currentRead[BUFFER_STRAIN_RATE]),
		m_dBuffers.getBufferData<BUFFER_POS>(posRead),
		m_dBuffers.getBufferData<BUFFER_VEL>(velRead),
		m_dBuffers.getBufferData<BUFFER_INFO>(gdata->currentRead[BUFFER_INFO]),
		m_dBuffers.getBufferData<BUFFER_HASH>(),
		m_dCellStart,
		m_dBuffers.getBufferData<BUFFER_NEIBSLIST>(),
		m_dBuffers.getBufferData<BUFFER_GRADGAMMA>(gdata->currentRead[BUFFER_GRADGAMMA]),
		m_dBuffers.getBufferData<BUFFER_BOUNDELEMENTS>(gdata->currentRead[BUFFER_BOUNDELEMENTS]),
		m_numParticles,
		numPartsToElaborate,
		m_simparams->slength,
		m_simparams->kerneltype,
		m_simparams->influenceRadius);
}


void GPUWorker::kernel_reduceRBForces()
{
	// is the device empty? (unlikely but possible before LB kicks in)
	if (m_numParticles == 0) {

		// make sure this device does not add any obsolete contribute to forces acting on objects
		for (uint ob = 0; ob < m_simparams->numODEbodies; ob++) {
			gdata->s_hRbTotalForce[m_deviceIndex][ob] = make_float3(0.0F);
			gdata->s_hRbTotalTorque[m_deviceIndex][ob] = make_float3(0.0F);
		}

		return;
	}

	reduceRbForces(m_dRbForces, m_dRbTorques, m_dRbNum, gdata->s_hRbLastIndex, gdata->s_hRbTotalForce[m_deviceIndex],
					gdata->s_hRbTotalTorque[m_deviceIndex], m_simparams->numODEbodies, m_numBodiesParticles);
}

void GPUWorker::kernel_updateValuesAtBoundaryElements()
{
	uint numPartsToElaborate = (gdata->only_internal ? m_particleRangeEnd : m_numParticles);

	// is the device empty? (unlikely but possible before LB kicks in)
	if (numPartsToElaborate == 0) return;

	// vel, tke, eps are read from current*Read, except
	// on the second step, whe they are read from current*Write
	bool secondStep = (gdata->commandFlags & INTEGRATOR_STEP_2);
	uint velRead = secondStep ? gdata->currentWrite[BUFFER_VEL] : gdata->currentRead[BUFFER_VEL];
	uint tkeRead = secondStep ? gdata->currentWrite[BUFFER_TKE] : gdata->currentRead[BUFFER_TKE];
	uint epsRead = secondStep ? gdata->currentWrite[BUFFER_EPSILON] : gdata->currentRead[BUFFER_EPSILON];

	updateBoundValues(
				m_dBuffers.getBufferData<BUFFER_VEL>(velRead),
				m_dBuffers.getBufferData<BUFFER_PRESSURE>(gdata->currentRead[BUFFER_PRESSURE]),
				m_dBuffers.getBufferData<BUFFER_TKE>(tkeRead),
				m_dBuffers.getBufferData<BUFFER_EPSILON>(epsRead),
				m_dBuffers.getBufferData<BUFFER_VERTICES>(gdata->currentRead[BUFFER_VERTICES]),
				m_dBuffers.getBufferData<BUFFER_INFO>(gdata->currentRead[BUFFER_INFO]),
				m_numParticles,
				numPartsToElaborate,
				true);
}

void GPUWorker::kernel_dynamicBoundaryConditions()
{
	uint numPartsToElaborate = (gdata->only_internal ? m_particleRangeEnd : m_numParticles);

	// is the device empty? (unlikely but possible before LB kicks in)
	if (numPartsToElaborate == 0) return;

	// pos, vel, tke, eps are read from current*Read, except
	// on the second step, whe they are read from current*Write
	bool secondStep = (gdata->commandFlags & INTEGRATOR_STEP_2);
	uint posRead = secondStep ? gdata->currentWrite[BUFFER_POS] : gdata->currentRead[BUFFER_POS];
	uint velRead = secondStep ? gdata->currentWrite[BUFFER_VEL] : gdata->currentRead[BUFFER_VEL];
	uint tkeRead = secondStep ? gdata->currentWrite[BUFFER_TKE] : gdata->currentRead[BUFFER_TKE];
	uint epsRead = secondStep ? gdata->currentWrite[BUFFER_EPSILON] : gdata->currentRead[BUFFER_EPSILON];

	dynamicBoundConditions(
				m_dBuffers.getBufferData<BUFFER_POS>(posRead),   // pos(n)
				m_dBuffers.getBufferData<BUFFER_VEL>(velRead),   // vel(n)
				m_dBuffers.getBufferData<BUFFER_PRESSURE>(gdata->currentRead[BUFFER_PRESSURE]),
				m_dBuffers.getBufferData<BUFFER_TKE>(tkeRead),
				m_dBuffers.getBufferData<BUFFER_EPSILON>(epsRead),
				m_dBuffers.getBufferData<BUFFER_INFO>(gdata->currentRead[BUFFER_INFO]),
				m_dBuffers.getBufferData<BUFFER_HASH>(),
				m_dCellStart,
				m_dBuffers.getBufferData<BUFFER_NEIBSLIST>(),
				m_numParticles,
				numPartsToElaborate,
				gdata->problem->m_deltap,
				m_simparams->slength,
				m_simparams->kerneltype,
				m_simparams->influenceRadius);
}

void GPUWorker::kernel_initGradGamma()
{
	uint numPartsToElaborate = (gdata->only_internal ? m_particleRangeEnd : m_numParticles);

	// is the device empty? (unlikely but possible before LB kicks in)
	if (numPartsToElaborate == 0) return;

	initGradGamma(	m_dBuffers.getBufferData<BUFFER_POS>(gdata->currentRead[BUFFER_POS]),
				m_dBuffers.getBufferData<BUFFER_POS>(gdata->currentWrite[BUFFER_POS]),
				m_dBuffers.getBufferData<BUFFER_VEL>(gdata->currentWrite[BUFFER_VEL]),
				m_dBuffers.getBufferData<BUFFER_INFO>(gdata->currentRead[BUFFER_INFO]),
				m_dBuffers.getBufferData<BUFFER_BOUNDELEMENTS>(gdata->currentRead[BUFFER_BOUNDELEMENTS]),
				m_dBuffers.getBufferData<BUFFER_GRADGAMMA>(gdata->currentWrite[BUFFER_GRADGAMMA]),
				m_dBuffers.getBufferData<BUFFER_HASH>(),
				m_dCellStart,
				m_dBuffers.getBufferData<BUFFER_NEIBSLIST>(),
				m_numParticles,
				numPartsToElaborate,
				gdata->problem->m_deltap,
				m_simparams->slength,
				m_simparams->influenceRadius,
				m_simparams->kerneltype);
}

void GPUWorker::kernel_updateGamma()
{
	uint numPartsToElaborate = (gdata->only_internal ? m_particleRangeEnd : m_numParticles);

	// is the device empty? (unlikely but possible before LB kicks in)
	if (numPartsToElaborate == 0) return;

	bool initStep = (gdata->commandFlags & INITIALIZATION_STEP);
	// during initStep we use currentVelRead, else currentVelWrite
	uint velRead = initStep ? gdata->currentRead[BUFFER_VEL] : gdata->currentWrite[BUFFER_VEL];

	updateGamma(m_dBuffers.getBufferData<BUFFER_POS>(gdata->currentRead[BUFFER_POS]),
				m_dBuffers.getBufferData<BUFFER_POS>(gdata->currentWrite[BUFFER_POS]),
				m_dBuffers.getBufferData<BUFFER_VEL>(velRead),
				m_dBuffers.getBufferData<BUFFER_INFO>(gdata->currentRead[BUFFER_INFO]),
				m_dBuffers.getBufferData<BUFFER_BOUNDELEMENTS>(gdata->currentRead[BUFFER_BOUNDELEMENTS]),
				m_dBuffers.getBufferData<BUFFER_GRADGAMMA>(gdata->currentRead[BUFFER_GRADGAMMA]),
				m_dBuffers.getBufferData<BUFFER_GRADGAMMA>(gdata->currentWrite[BUFFER_GRADGAMMA]),
				m_dBuffers.getBufferData<BUFFER_HASH>(),
				m_dCellStart,
				m_dBuffers.getBufferData<BUFFER_NEIBSLIST>(),
				m_numParticles,
				numPartsToElaborate,
				m_simparams->slength,
				m_simparams->influenceRadius,
				gdata->extraCommandArg,
				!initStep, // 0 during init step, else 1
				m_simparams->kerneltype);
}

void GPUWorker::kernel_updatePositions()
{
	uint numPartsToElaborate = (gdata->only_internal ? m_particleRangeEnd : m_numParticles);

	// is the device empty? (unlikely but possible before LB kicks in)
	if (numPartsToElaborate == 0) return;

	updatePositions(m_dBuffers.getBufferData<BUFFER_POS>(gdata->currentRead[BUFFER_POS]),
					m_dBuffers.getBufferData<BUFFER_POS>(gdata->currentWrite[BUFFER_POS]),
					m_dBuffers.getBufferData<BUFFER_VEL>(gdata->currentRead[BUFFER_VEL]),
					m_dBuffers.getBufferData<BUFFER_INFO>(gdata->currentRead[BUFFER_INFO]),
					gdata->extraCommandArg,
					m_numParticles,
					numPartsToElaborate);
}

void GPUWorker::uploadConstants()
{
	// NOTE: visccoeff must be set before uploading the constants. This is done in GPUSPH main cycle

	// Setting kernels and kernels derivative factors
	setforcesconstants(m_simparams, m_physparams, gdata->worldOrigin, gdata->gridSize, gdata->cellSize,
		m_numAllocatedParticles);
	seteulerconstants(m_physparams, gdata->worldOrigin, gdata->gridSize, gdata->cellSize);
	setneibsconstants(m_simparams, m_physparams, gdata->worldOrigin, gdata->gridSize, gdata->cellSize,
		m_numAllocatedParticles);
}

void GPUWorker::uploadBodiesCentersOfGravity()
{
	setforcesrbcg(gdata->s_hRbGravityCenters, m_simparams->numODEbodies);
	seteulerrbcg(gdata->s_hRbGravityCenters, m_simparams->numODEbodies);
}

void GPUWorker::uploadBodiesTransRotMatrices()
{
	seteulerrbtrans(gdata->s_hRbTranslations, m_simparams->numODEbodies);
	seteulerrbsteprot(gdata->s_hRbRotationMatrices, m_simparams->numODEbodies);
}
