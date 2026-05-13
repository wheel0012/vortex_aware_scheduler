//========================================================================================================================================================================================================200
//	DEFINE/INCLUDE
//========================================================================================================================================================================================================200

//======================================================================================================================================================150
//	DEFINE
//======================================================================================================================================================150

// clBuildProgram compiler cannot link this file for some reason, so had to redefine constants and structures below
// #include ../common.h						// (in directory specified to compiler)			main function header

//======================================================================================================================================================150
//	DEFINE (had to bring from ../common.h here because feature of including headers in clBuildProgram does not work for some reason)
//======================================================================================================================================================150

// change to double if double precision needed
#define fp float

//#define DEFAULT_ORDER_2 256

//======================================================================================================================================================150
//	STRUCTURES (had to bring from ../common.h here because feature of including headers in clBuildProgram does not work for some reason)
//======================================================================================================================================================150

// ???
typedef struct knode {
	int location;
	int indices [DEFAULT_ORDER_2 + 1];
	int  keys [DEFAULT_ORDER_2 + 1];
	char is_leaf;
	int num_keys;
} knode; 

//========================================================================================================================================================================================================200
//	findRangeK function
//========================================================================================================================================================================================================200

__kernel void 
findRangeK(	int height,
			__global knode *knodesD,
			int knodes_elem,

			__global int *currKnodeD,
			__global int *offsetD,
			__global int *lastKnodeD,
			__global int *offset_2D,
			__global int *startD,
			__global int *endD,
			__global int *RecstartD, 
			__global int *ReclenD)
{
	int bid = get_global_id(0);
	int curr = currKnodeD[bid];
	int next = offsetD[bid];
	int last = lastKnodeD[bid];
	int next_last = offset_2D[bid];

	for (int level = 0; level < height; level++) {
		for (int slot = 0; slot < DEFAULT_ORDER_2; slot++) {
			if (knodesD[curr].keys[slot] <= startD[bid] &&
				knodesD[curr].keys[slot + 1] > startD[bid] &&
				knodesD[curr].indices[slot] < knodes_elem) {
				next = knodesD[curr].indices[slot];
			}
			if (knodesD[last].keys[slot] <= endD[bid] &&
				knodesD[last].keys[slot + 1] > endD[bid] &&
				knodesD[last].indices[slot] < knodes_elem) {
				next_last = knodesD[last].indices[slot];
			}
		}
		curr = next;
		last = next_last;
	}

	currKnodeD[bid] = curr;
	offsetD[bid] = next;
	lastKnodeD[bid] = last;
	offset_2D[bid] = next_last;
	for (int slot = 0; slot < DEFAULT_ORDER_2; slot++) {
		if (knodesD[curr].keys[slot] == startD[bid]) {
			RecstartD[bid] = knodesD[curr].indices[slot];
		}
	}
	for (int slot = 0; slot < DEFAULT_ORDER_2; slot++) {
		if (knodesD[last].keys[slot] == endD[bid]) {
			ReclenD[bid] = knodesD[last].indices[slot] - RecstartD[bid] + 1;
		}
	}
}

//========================================================================================================================================================================================================200
//	End
//========================================================================================================================================================================================================200
