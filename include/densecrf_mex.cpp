// Solve the complete problem with either mean-field (MF), or tree-reweighted message passings (TRWS)
#include "meanfield.h"
#include "solvers.h"
double timer;

// Catch errors
static void erfunc(char *err) {
  mexErrMsgTxt(err);
}


void mexFunction(int nlhs, 		    /* number of expected outputs */
        mxArray        *plhs[],	    /* mxArray output pointer array */
        int            nrhs, 		/* number of inputs */
        const mxArray  *prhs[]		/* mxArray input pointer array */)
{
    startTime();

    // Parsing data from MATLAB
    if (nrhs != 4)
        mexErrMsgTxt("Expected 3 inputs");
    
    const matrix<unsigned char> im_matrix(prhs[0]);
    const matrix<float>  unary_matrix(prhs[1]);
    const matrix<unsigned int> im_size(prhs[2]);

    
    //Structure to hold and parse additional parameters
    MexParams params(1, prhs+3);
    
    // Weights used to define the energy function
    PairwiseWeights pairwiseWeights(params);
    const bool debug = params.get<bool>("debug", false);
    const int iterations = params.get<int>("iterations", 20);  

    // Used only for TRW-S
    const double min_pairwise_cost = params.get<double>("min_pairwise_cost", 0);

    // Redefine the problem like Krähenbühl
    string solver = params.get<string>("solver", "Not set");      

    // The image
    const int M = im_size(0);
    const int N = im_size(1);
    const int C = im_size(2);
    const int numVariables = M*N;

    // Calculate number of labels
    const int UC = unary_matrix.numel();
    const size_t numberOfLabels = UC/(M*N);
    
    // Read image and unary
    const unsigned char * image  = im_matrix.data;
    float * unary_array  = unary_matrix.data;
 
    //support solvers MF    - Efficient Inference in Fully Connected CRFs with gaussian edge Potential
    //                TRWS  - sequential tree-reweighted message passing
    assert(M > 0);
    assert(N > 0);


    if (solver.compare("MF") && solver.compare("TRWS")) {
        mexErrMsgTxt("Unknown solver");
    }
     
    // Oracle function to get cost
    LinearIndex unaryLinearIndex(M,N, numberOfLabels);
    LinearIndex imageLinearIndex(M,N, C);
    
    Linear2sub linear2sub(M,N);
    UnaryCost unaryCost(unary_array, unaryLinearIndex);
   
    if (debug)
    {
      mexPrintf("min_pairwise_cost: %g \n", min_pairwise_cost);
      mexPrintf("Problem size: %d x %d \n", M,N);

      endTime("Reading data.");
    }
      
    matrix<double> result(M,N);
    matrix<double> energy(1);
    matrix<double> bound(1);
     
    plhs[0] = result;
    plhs[1] = energy;
    plhs[2] = bound; 

    PairwiseCost pairwiseCost(image, pairwiseWeights, imageLinearIndex);
    EnergyFunctor energyFunctor(unaryCost, pairwiseCost, M,N, numberOfLabels);

    // Mean-field
    if(!solver.compare("MF"))
    {
      // Setup the CRF model
      extendedDenseCRF2D crf(M,N,numberOfLabels);
      Map<MatrixXf> unary(unary_array, numberOfLabels, numVariables);

      crf.setUnaryEnergy( unary );
          
      KernelType kerneltype = CONST_KERNEL;
      NormalizationType normalizationtype = parseNormalizationType(params);

      // Setup  pairwise cost
      crf.addPairwiseGaussian(pairwiseWeights.gaussian_x_stddev, 
                              pairwiseWeights.gaussian_y_stddev, 
                              new PottsCompatibility(pairwiseWeights.gaussian_weight),
                              kerneltype,
                              normalizationtype);
    
      crf.addPairwiseBilateral(pairwiseWeights.bilateral_x_stddev, 
                               pairwiseWeights.bilateral_y_stddev,
                               pairwiseWeights.bilateral_r_stddev, 
                               pairwiseWeights.bilateral_g_stddev, 
                               pairwiseWeights.bilateral_b_stddev,
                               image,
                               new PottsCompatibility(pairwiseWeights.bilateral_weight),
                               kerneltype,
                               normalizationtype);
          
      // Do map inference
      VectorXs map = crf.map(iterations);

      //Packing data in the same way as input
      for (int i = 0; i < numVariables; i++ ) {
           result(i) = (double)map[i];
      }
      
      energy(0) = crf.energy(map);
      bound(0) = lowestUnaryCost( unary_array, M,N,numberOfLabels );
 
      if (debug)
        endTime("Solving with MF.");
      
      return;
    }

    if(!solver.compare("TRWS")) 
    {
      TypePotts::REAL TRWSenergy, TRWSlb;
      TRWSOptions options;
      options.m_iterMax = iterations;

      if (!debug)
        options.m_printMinIter = iterations+2;

      TRWS * mrf = new TRWS(TypePotts::GlobalSize(numberOfLabels),erfunc);
      TRWSNodes * nodes = new TRWSNodes[numVariables];

      TypePotts::REAL D[numberOfLabels];

      for (int i = 0; i < numVariables; i++) 
      {
        for(int s = 0; s < numberOfLabels; ++s) 
        {          
          std::pair<int,int> p = linear2sub(i);
          D[s] = unaryCost( p, s );
        }

        nodes[i] = mrf->AddNode(TypePotts::LocalSize(), 
                                TypePotts::NodeData(D));
      }

      // Pairwise cost 
      for (size_t i = 0; i < numVariables; i++) {
      for (size_t j = i+1; j < numVariables; j++) {  
        std::pair<int,int> p0 = linear2sub(i);
        std::pair<int,int> p1 = linear2sub(j);

        double pcost = pairwiseCost( p0, p1 );

        if (pcost >= min_pairwise_cost)
            mrf->AddEdge(nodes[i], nodes[j], TypePotts::EdgeData(pcost));
      }
      }

      mrf->Minimize_TRW_S(options, TRWSlb, TRWSenergy);

      for (int i = 0; i < numVariables; i++)
        result(i) = mrf->GetSolution(nodes[i]);

      energy(0) = TRWSenergy;
      bound(0) = TRWSlb;

      delete mrf;
      delete nodes;

      if (debug)
        endTime("Solving with TRWS.");

      return;
    }
}