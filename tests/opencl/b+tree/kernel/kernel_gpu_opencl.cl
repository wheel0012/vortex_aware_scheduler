// #ifdef __cplusplus
// extern "C" {
// #endif

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

//#define DEFAULT_ORDER 256

//======================================================================================================================================================150
//	STRUCTURES (had to bring from ../common.h here because feature of including headers in clBuildProgram does not work for some reason)
//======================================================================================================================================================150

// Type representing the record to which a given key refers. In a real B+ tree system, the record would hold data (in a database) or a file (in an operating system) or some other information.
// Users can rewrite this part of the code to change the type and content of the value field.
typedef struct record {
	int value;
} record;

// ???
typedef struct knode {
	int location;
	int indices [DEFAULT_ORDER + 1];
	int  keys [DEFAULT_ORDER + 1];
	char is_leaf;
	int num_keys;
} knode; 

//========================================================================================================================================================================================================200
//	findK function
//========================================================================================================================================================================================================200

__kernel void 
findK(	int height,
		__global knode *knodesD,
		int knodes_elem,
		__global record *recordsD,

		__global int *currKnodeD,
		__global int *offsetD,
		__global int *keysD, 
		__global record *ansD)
{
	int bid = get_global_id(0);
	int curr = currKnodeD[bid];
	int next = offsetD[bid];

	for (int level = 0; level < height; level++) {
		for (int slot = 0; slot < DEFAULT_ORDER; slot++) {
			if (knodesD[curr].keys[slot] <= keysD[bid] &&
				knodesD[curr].keys[slot + 1] > keysD[bid] &&
				knodesD[next].indices[slot] < knodes_elem) {
				next = knodesD[next].indices[slot];
			}
		}
		curr = next;
	}

	currKnodeD[bid] = curr;
	offsetD[bid] = next;
	for (int slot = 0; slot < DEFAULT_ORDER; slot++) {
		if (knodesD[curr].keys[slot] == keysD[bid]) {
			ansD[bid].value = recordsD[knodesD[curr].indices[slot]].value;
		}
	}
}

//========================================================================================================================================================================================================200
//	End
//========================================================================================================================================================================================================200

// #ifdef __cplusplus
// }
// #endif
