// See LICENCE file at project root
#ifndef FFMMALGORITHMTESTTASK_HPP
#define FFMMALGORITHMTESTTASK_HPP

#include <algorithm>
#include <array>

#include <omp.h>

#include "Utils/FAlgorithmTimers.hpp"
#include "Utils/FAssert.hpp"
#include "Utils/FEnv.hpp"

#include "Utils/FGlobal.hpp"
#include "Utils/FLog.hpp"

#include "Utils/FTic.hpp"

#include "Containers/FOctree.hpp"
#include "Containers/FVector.hpp"

#include "Components/FBasicCell.hpp"

#include "FCoreCommon.hpp"
#include "FP2PExclusion.hpp"

#include <omp.h>

/**
 * @authorOlivier Coulaud 
 * @class FFmmAlgorithmNewTask
 * @brief
 *
 * Please read the license
 *
 * This class is a basic FMM algorithm
 * It just iterates on a tree and call the kernels with good arguments.
 *
 * Of course this class does not deallocate pointer given in arguements.
 */
template<class OctreeClass, class CellClass, class ContainerClass, class KernelClass, class LeafClass, class P2PExclusionClass = FP2PMiddleExclusion>
class FFmmAlgorithmTestTask : public FAbstractAlgorithm, public FAlgorithmTimers {

    using multipole_t       = typename CellClass::multipole_t;
    using local_expansion_t = typename CellClass::local_expansion_t;
    using symbolic_data_t   = CellClass;

    OctreeClass* const tree;       //< The octree to work on
    KernelClass** kernels;    //< The kernels

    int MaxThreads;

    const int OctreeHeight;

    const int leafLevelSeparationCriteria;
public:
    /** The constructor need the octree and the kernels used for computation
     * @param inTree the octree to work on
     * @param inKernels the kernels to call
     * An assert is launched if one of the arguments is null
     */
    FFmmAlgorithmTestTask(OctreeClass* const inTree, const KernelClass* const inKernels, const int inLeafLevelSeperationCriteria = 1)
        : tree(inTree) , kernels(nullptr),
          OctreeHeight(tree->getHeight()), leafLevelSeparationCriteria(inLeafLevelSeperationCriteria)
    {

        FAssertLF(tree, "tree cannot be null");
        FAssertLF(inKernels, "kernels cannot be null");
        FAssertLF(leafLevelSeparationCriteria < 3, "Separation criteria should be < 3");

        MaxThreads = 1;
        #pragma omp parallel
        #pragma omp master
            MaxThreads = omp_get_num_threads();

        this->kernels = new KernelClass*[MaxThreads];
        #pragma omp parallel num_threads(MaxThreads)
        {
            #pragma omp critical (InitFFmmAlgorithmTestTask)
            {
                this->kernels[omp_get_thread_num()] = new KernelClass(*inKernels);
            }
        }

        FAbstractAlgorithm::setNbLevelsInTree(tree->getHeight());

        FLOG(FLog::Controller << "FFmmAlgorithmTestTask (Max Thread " << omp_get_num_threads() << ")\n");
    }

    /** Default destructor */
    virtual ~FFmmAlgorithmTestTask(){
        for(int idxThread = 0 ; idxThread < MaxThreads ; ++idxThread){
            delete this->kernels[idxThread];
        }
        delete [] this->kernels;
    }

    std::string name() const override {
        return "Test Task algorithm";
    }

    std::string description() const override {
        int threads = 1;
        #pragma omp parallel shared(threads)
        {
            #pragma omp single nowait
            {
                threads = omp_get_num_threads();
            }
        }
        return std::string("threads: ") + std::to_string(threads);
    }

protected:
    /**
     * To execute the fmm algorithm
     * Call this function to run the complete algorithm
     */
    void executeCore(const unsigned operationsToProceed) override {

      #pragma omp parallel num_threads(MaxThreads) 
      {
        #pragma omp single nowait
        {
          if(operationsToProceed & FFmmP2M) bottomPass();

          if(operationsToProceed & FFmmM2M) upwardPass();

          if(operationsToProceed & FFmmM2L) transferPass();

          if(operationsToProceed & FFmmL2L) downardPass();
        }

        #pragma omp single nowait
        { 
          if( operationsToProceed & FFmmP2P ) directPass();
        }
        #pragma omp barrier

        #pragma omp single nowait
        {
          if( operationsToProceed & FFmmL2P ) L2PPass() ; 
        }
      }
    }
    /////////////////////////////////////////////////////////////////////////////
    // P2M
    /////////////////////////////////////////////////////////////////////////////

    /** P2M */
    void bottomPass(){
        FLOG( FLog::Controller.write("\tStart Bottom Pass\n").write(FLog::Flush) );
        FLOG(FTic counterTime);

	//        #pragma omp parallel num_threads(MaxThreads)
        {
      //            #pragma omp single nowait
            {
                typename OctreeClass::Iterator octreeIterator(tree);

                // Iterate on leafs
                octreeIterator.gotoBottomLeft();
                do{
                    // We need the current cell that represent the leaf
                    // and the list of particles
                    #pragma omp task firstprivate(octreeIterator) untied
                    {
                        kernels[omp_get_thread_num()]->P2M(
                            &(octreeIterator.getCurrentCell()->getMultipoleData()),
                            octreeIterator.getCurrentCell(),
                            octreeIterator.getCurrentListSrc());
                    }
                } while(octreeIterator.moveRight());

                #pragma omp taskwait
            }
        }

        FLOG( FLog::Controller << "\tFinished (@Bottom Pass (P2M) = "  << counterTime.tacAndElapsed() << " s)\n" );
    }

    /////////////////////////////////////////////////////////////////////////////
    // Upward
    /////////////////////////////////////////////////////////////////////////////

    /** M2M */
    void upwardPass(){
        FLOG( FLog::Controller.write("\tStart Upward Pass\n").write(FLog::Flush); );
        FLOG(FTic counterTime);

	//        #pragma omp parallel num_threads(MaxThreads)
        {
	  //            #pragma omp single nowait
            {
                // Start from leal level - 1
                typename OctreeClass::Iterator octreeIterator(tree);
                octreeIterator.gotoBottomLeft();
                octreeIterator.moveUp();

                for(int idxLevel = OctreeHeight - 2 ; idxLevel > FAbstractAlgorithm::lowerWorkingLevel-1 ; --idxLevel){
                    octreeIterator.moveUp();
                }

                typename OctreeClass::Iterator avoidGotoLeftIterator(octreeIterator);

                // for each levels
                for(int idxLevel = FMath::Min(OctreeHeight - 2, FAbstractAlgorithm::lowerWorkingLevel - 1) ; idxLevel >= FAbstractAlgorithm::upperWorkingLevel ; --idxLevel ){
                    FLOG(FTic counterTimeLevel);
                    // for each cells
                    do{
                        // We need the current cell and its children.
                        // children is an array (of 8 child) that may be null
                        #pragma omp task firstprivate(octreeIterator,idxLevel) untied
                        {
                            multipole_t* const parent_multipole
                                = &(octreeIterator.getCurrentCell()->getMultipoleData());
                            const symbolic_data_t* const parent_symbolic
                                = octreeIterator.getCurrentCell();

                            CellClass** children = octreeIterator.getCurrentChildren();
                            std::array<const multipole_t*, 8> child_multipoles;
                            std::transform(children, children+8, child_multipoles.begin(),
                                           [](CellClass* c) {
                                               return (c == nullptr ? nullptr
                                                       : &(c->getMultipoleData()));
                                           });
                            std::array<const symbolic_data_t*, 8> child_symbolics;
                            std::transform(children, children+8, child_symbolics.begin(),
                                           [](CellClass* c) {return c;});
                            kernels[omp_get_thread_num()]->M2M(parent_multipole,
                                                               parent_symbolic,
                                                               child_multipoles.data(),
                                                               child_symbolics.data());
                        }
                    } while(octreeIterator.moveRight());

                    avoidGotoLeftIterator.moveUp();
                    octreeIterator = avoidGotoLeftIterator;// equal octreeIterator.moveUp(); octreeIterator.gotoLeft();

                    #pragma omp taskwait
                    FLOG( FLog::Controller << "\t\t>> Level " << idxLevel << " = "  << counterTimeLevel.tacAndElapsed() << " s\n" );
                }
            }
        }

        FLOG( FLog::Controller << "\tFinished (@Upward Pass (M2M) = "  << counterTime.tacAndElapsed() << " s)\n" );
    }

    /////////////////////////////////////////////////////////////////////////////
    // Transfer
    /////////////////////////////////////////////////////////////////////////////

    /** M2L  */
    void transferPass(){
        #ifdef SCALFMM_USE_EZTRACE

        eztrace_start();
        #endif
        if(KernelClass::NeedFinishedM2LEvent()){
            this->transferPassWithFinalize() ;
        }
        else{
            this->transferPassWithoutFinalize() ;
        }
        #ifdef SCALFMM_USE_EZTRACE
        eztrace_stop();
        #endif
    }

    void transferPassWithoutFinalize(){
        FLOG( FLog::Controller.write("\tStart Downward Pass (M2L)\n").write(FLog::Flush); );
        FLOG(FTic counterTime);

	//        #pragma omp parallel num_threads(MaxThreads)
        {
	  //            #pragma omp single nowait
            {
                typename OctreeClass::Iterator octreeIterator(tree);
                // Goto the right level
                octreeIterator.moveDown();
                for(int idxLevel = 2 ; idxLevel < FAbstractAlgorithm::upperWorkingLevel ; ++idxLevel){
                    octreeIterator.moveDown();
                }
                ////////////////////////////////////////////////////////////////
                typename OctreeClass::Iterator avoidGotoLeftIterator(octreeIterator);
                //
                // for each levels
                for(int idxLevel = FAbstractAlgorithm::upperWorkingLevel ; idxLevel < FAbstractAlgorithm::lowerWorkingLevel ; ++idxLevel ){
                    FLOG(FTic counterTimeLevel);
                    const int separationCriteria = (idxLevel != FAbstractAlgorithm::lowerWorkingLevel-1 ? 1 : leafLevelSeparationCriteria);
                    // for each cell we apply the M2L with all cells in the implicit interaction list
                    do{
                        #pragma omp task firstprivate(octreeIterator,idxLevel) untied
                        {
                            const CellClass* neighbors[342];
                            int neighborPositions[342];
                            const int counter = tree->getInteractionNeighbors(
                                neighbors, neighborPositions,
                                octreeIterator.getCurrentGlobalCoordinate(),
                                idxLevel, separationCriteria);

                            if(counter) {
                                local_expansion_t* const target_local_exp
                                    = &(octreeIterator.getCurrentCell()->getLocalExpansionData());
                                const symbolic_data_t* const target_symbolic
                                    = octreeIterator.getCurrentCell();
                                std::array<const multipole_t*, 342> neighbor_multipoles;
                                std::transform(neighbors, neighbors+counter, neighbor_multipoles.begin(),
                                               [](const CellClass* c) {
                                                   return (c == nullptr ? nullptr
                                                           : &(c->getMultipoleData()));
                                               });
                                std::array<const symbolic_data_t*, 342> neighbor_symbolics;
                                std::transform(neighbors, neighbors+counter, neighbor_symbolics.begin(),
                                               [](const CellClass* c) {return c;});

                                kernels[omp_get_thread_num()]->M2L(
                                    target_local_exp,
                                    target_symbolic,
                                    neighbor_multipoles.data(),
                                    neighbor_symbolics.data(),
                                    neighborPositions,
                                    counter);
                            }
                        }

                    } while(octreeIterator.moveRight());
                    ////////////////////////////////////////////////////////////////
                    // move up  and goto left
                    avoidGotoLeftIterator.moveDown();
                    octreeIterator = avoidGotoLeftIterator;

                    FLOG( FLog::Controller << "\t\t>> Level " << idxLevel << " = "  << counterTimeLevel.tacAndElapsed() << " s\n" );
                }
            }
        }  // end parallel region
        //
        FLOG( FLog::Controller << "\tFinished (@Downward Pass (M2L) = "  << counterTime.tacAndElapsed() << " s)\n" );

    }
    void transferPassWithFinalize(){

        FLOG( FLog::Controller.write("\tStart Downward Pass (M2L)\n").write(FLog::Flush); );
        FLOG(FTic counterTime);
	//        #pragma omp parallel num_threads(MaxThreads)
        {
          //  #pragma omp single nowait
            {

                typename OctreeClass::Iterator octreeIterator(tree);
                octreeIterator.moveDown();

                for(int idxLevel = 2 ; idxLevel < FAbstractAlgorithm::upperWorkingLevel ; ++idxLevel){
                    octreeIterator.moveDown();
                }

                typename OctreeClass::Iterator avoidGotoLeftIterator(octreeIterator);
                // FIXME: hack around a clang bug
                // it apparently can't manage a firstprivate const member
                // such as 'tree', but can manage a local copy...
                // getting the height first, or making 'tree' shared both
                // workaround it.
                OctreeClass * const treeAlias = tree;

                // for each levels
                for(int idxLevel = FAbstractAlgorithm::upperWorkingLevel ; idxLevel < FAbstractAlgorithm::lowerWorkingLevel ; ++idxLevel ){
                    FLOG(FTic counterTimeLevel);
                    const int separationCriteria = (idxLevel != FAbstractAlgorithm::lowerWorkingLevel-1 ? 1 : leafLevelSeparationCriteria);
                    // for each cells
                    do{
                        #pragma omp task default(none) firstprivate(octreeIterator,separationCriteria,idxLevel,treeAlias,kernels) untied
                        {
                            const CellClass* neighbors[342];
                            int neighborPositions[342];
                            const int counter = treeAlias->getInteractionNeighbors(
                                neighbors, neighborPositions,
                                octreeIterator.getCurrentGlobalCoordinate(),
                                idxLevel, separationCriteria);


                            if(counter) {

                                local_expansion_t* const target_local_exp
                                    = &(octreeIterator.getCurrentCell()->getLocalExpansionData());
                                const symbolic_data_t* const target_symbolic
                                    = octreeIterator.getCurrentCell();
                                std::array<const multipole_t*, 342> neighbor_multipoles;
                                std::transform(neighbors, neighbors+counter, neighbor_multipoles.begin(),
                                                                             FBasicCell::getMultipoleDataFromCell<const CellClass, const multipole_t>);

                                std::array<const symbolic_data_t*, 342> neighbor_symbolics;
                                std::transform(neighbors, neighbors+counter, neighbor_symbolics.begin(),
                                                                             FBasicCell::identity<const CellClass>);

                                kernels[omp_get_thread_num()]->M2L(
                                    target_local_exp,
                                    target_symbolic,
                                    neighbor_multipoles.data(),
                                    neighbor_symbolics.data(),
                                    neighborPositions,
                                    counter);
                            }
                        }

                    } while(octreeIterator.moveRight());

                    avoidGotoLeftIterator.moveDown();
                    octreeIterator = avoidGotoLeftIterator;

                    #pragma omp taskwait

                    for( int idxThread = 0 ; idxThread < omp_get_num_threads() ; ++idxThread){
                        #pragma omp task
                        {
                            kernels[idxThread]->finishedLevelM2L(idxLevel);
                        }
                    }
                    #pragma omp taskwait
                    FLOG( FLog::Controller << "\t\t>> Level " << idxLevel << " = "  << counterTimeLevel.tacAndElapsed() << " s\n" );
                }
            } // end single region
        } // end // region
        FLOG( FLog::Controller << "\tFinished (@Downward Pass (M2L) = "  << counterTime.tacAndElapsed() << " s)\n" );
    }

    /////////////////////////////////////////////////////////////////////////////
    // Downward
    /////////////////////////////////////////////////////////////////////////////

    void downardPass(){ // second L2L
        FLOG( FLog::Controller.write("\tStart Downward Pass (L2L)\n").write(FLog::Flush); );
        FLOG(FTic counterTime);

	//        #pragma omp parallel num_threads(MaxThreads)
        {
	  //            #pragma omp single nowait
            {
                typename OctreeClass::Iterator octreeIterator(tree);
                octreeIterator.moveDown();

                for(int idxLevel = 2 ; idxLevel < FAbstractAlgorithm::upperWorkingLevel ; ++idxLevel){
                    octreeIterator.moveDown();
                }

                typename OctreeClass::Iterator avoidGotoLeftIterator(octreeIterator);

                const int heightMinusOne = FAbstractAlgorithm::lowerWorkingLevel - 1;
                // for each levels exepted leaf level
                for(int idxLevel = FAbstractAlgorithm::upperWorkingLevel ; idxLevel < heightMinusOne ; ++idxLevel ){
                    FLOG(FTic counterTimeLevel);
                    // for each cells
                    do{
                        #pragma omp task firstprivate(octreeIterator,idxLevel) untied
                        {
                            local_expansion_t* const parent_local_exp
                                = &(octreeIterator.getCurrentCell()->getLocalExpansionData());
                            const symbolic_data_t* const parent_symbolic
                                = octreeIterator.getCurrentCell();
                            CellClass** children = octreeIterator.getCurrentChildren();
                            std::array<local_expansion_t*, 8> child_local_expansions;
                            std::transform(children, children+8, child_local_expansions.begin(),
                                           [](CellClass* c) {return (c == nullptr ? nullptr
                                                                     : &(c->getLocalExpansionData()));
                                           });
                            std::array<symbolic_data_t*, 8> child_symbolics;
                            std::transform(children, children+8, child_symbolics.begin(),
                                           [](CellClass* c) {return c;});
                            kernels[omp_get_thread_num()]->L2L(
                                parent_local_exp,
                                parent_symbolic,
                                child_local_expansions.data(),
                                child_symbolics.data()
                                );
                        }

                    } while(octreeIterator.moveRight());

                    avoidGotoLeftIterator.moveDown();
                    octreeIterator = avoidGotoLeftIterator;

                    #pragma omp taskwait
                    FLOG( FLog::Controller << "\t\t>> Level " << idxLevel << " = "  << counterTimeLevel.tacAndElapsed() << " s\n" );
                }
            }
        }

        FLOG( FLog::Controller << "\tFinished (@Downward Pass (L2L) = "  << counterTime.tacAndElapsed() << " s)\n" );
    }


    /////////////////////////////////////////////////////////////////////////////
    // Direct
    /////////////////////////////////////////////////////////////////////////////

    /** P2P */
    void directPass(){
        FLOG( FLog::Controller.write("\tStart Direct Pass\n").write(FLog::Flush); );
        FLOG(FTic counterTime);
        FLOG(FTic computationCounter);

        const int heightMinusOne = OctreeHeight - 1;

	//        #pragma omp parallel num_threads(MaxThreads)
        {

          //  #pragma omp single nowait
            {

                const int SizeShape = P2PExclusionClass::SizeShape;
                FVector<typename OctreeClass::Iterator> shapes[SizeShape];

                typename OctreeClass::Iterator octreeIterator(tree);
                octreeIterator.gotoBottomLeft();

                // for each leafs
                do{
                    const FTreeCoordinate& coord = octreeIterator.getCurrentGlobalCoordinate();
                    const int shapePosition = P2PExclusionClass::GetShapeIdx(coord);

                    shapes[shapePosition].push(octreeIterator);

                } while(octreeIterator.moveRight());

                FLOG( computationCounter.tic() );

                for( int idxShape = 0 ; idxShape < SizeShape ; ++idxShape){
		  const FSize nbLeaf = (shapes[idxShape].getSize());
		  for(FSize iterLeaf = 0 ; iterLeaf < nbLeaf ; ++iterLeaf ){
		    typename OctreeClass::Iterator toWork = shapes[idxShape][iterLeaf];
#pragma omp task firstprivate(toWork) untied
		    {
		      // There is a maximum of 26 neighbors
		      ContainerClass* neighbors[26];
		      int neighborPositions[26];
		      const int counter = tree->getLeafsNeighbors(neighbors, neighborPositions, toWork.getCurrentGlobalCoordinate(),heightMinusOne);
		      kernels[omp_get_thread_num()]->P2P(toWork.getCurrentGlobalCoordinate(), toWork.getCurrentListTargets(),
							 toWork.getCurrentListSrc(), neighbors, neighborPositions, counter);
		    }
		  }

#pragma omp taskwait
                }
		
                FLOG( computationCounter.tac() );
            }
        }


        FLOG( FLog::Controller << "\tFinished (@Direct Pass (L2P + P2P) = "  << counterTime.tacAndElapsed() << " s)\n" );
        FLOG( FLog::Controller << "\t\t Computation L2P + P2P : " << computationCounter.cumulated() << " s\n" );
    }
    
    void L2PPass(){
        FLOG( FLog::Controller.write("\tStart L2P Pass\n").write(FLog::Flush); );
        FLOG(FTic counterTime);

        typename OctreeClass::Iterator octreeIterator(tree);
        octreeIterator.gotoBottomLeft();

        // for each leafs
        do{
            #pragma omp task firstprivate(octreeIterator) untied
            {
                kernels[omp_get_thread_num()]->L2P(
                    &(octreeIterator.getCurrentCell()->getLocalExpansionData()),
                    octreeIterator.getCurrentCell(),
                    octreeIterator.getCurrentListTargets());
            }
        } while(octreeIterator.moveRight());

        #pragma omp taskwait

        FLOG( FLog::Controller << "\tFinished (@Direct Pass (L2P) = "  << counterTime.tacAndElapsed() << " s)\n" );
    }
};


#endif //FFMMALGORITHMTASK_HPP
