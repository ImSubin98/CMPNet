/**
# override the ompl rrt planner to implement mpnet planner
**/
/* Author: Yinglong Miao */

#include "ompl/geometric/planners/rrt/RRT.h"
#include <limits>
#include "ompl/base/goals/GoalSampleableRegion.h"
#include "ompl/tools/config/SelfConfig.h"
#include "ompl/base/Goal.h"
#include "ompl/base/goals/GoalSampleableRegion.h"
#include "ompl/base/goals/GoalState.h"
#include "ompl/base/objectives/PathLengthOptimizationObjective.h"
#include "ompl/base/samplers/InformedStateSampler.h"
#include "ompl/base/samplers/informed/RejectionInfSampler.h"
#include "ompl/base/samplers/informed/OrderedInfSampler.h"
#include "ompl/tools/config/SelfConfig.h"
#include "ompl/util/GeometricEquations.h"
#include "ompl/base/spaces/RealVectorStateSpace.h"
#include <ompl/base/goals/GoalStates.h>

#include <torch/torch.h>
#include <torch/script.h>
#include "mpnet_planner.hpp"
#include <iostream>
#include <cmath>
#include <iterator>
using namespace ompl;
using namespace std;
#define DEBUG

MPNetPlanner::MPNetPlanner(const base::SpaceInformationPtr &si, bool addIntermediateStates, int max_replan, int max_length)
  : base::Planner(si, addIntermediateStates ? "MPNetPlannerintermediate" : "MPNetPlanner")
  , _max_replan(max_replan)  // in exp: we use 1001
  , _max_length(max_length)  // in exp: we use 3000
{
    specs_.approximateSolutions = true;
    specs_.directed = true;

    Planner::declareParam<double>("range", this, &MPNetPlanner::setRange, &MPNetPlanner::getRange, "0.:1.:10000.");
    Planner::declareParam<double>("goal_bias", this, &MPNetPlanner::setGoalBias, &MPNetPlanner::getGoalBias, "0.:.05:1.");
    Planner::declareParam<bool>("intermediate_states", this, &MPNetPlanner::setIntermediateStates, &MPNetPlanner::getIntermediateStates,
                                "0,1");

    addIntermediateStates_ = addIntermediateStates;

    // state information
    for (int i=0; i < 3; i++)
    {
        bound[i] = (upper_bound[i]-lower_bound[i]) / 2;
    }

    // MPNet specific: load network structure and parameters
    std::shared_ptr<torch::jit::script::Module> encoder_ptr = torch::jit::load("../encoder_annotated_test_cpu_2.pt"));
    std::shared_ptr<torch::jit::script::Module> mlp_ptr = torch::jit::load("../mlp_annotated_test_gpu_2.pt"));
    encoder = encoder_ptr;
    MLP = mlp_ptr;
    MLP->to(at::kCUDA);

    // obtain obstacle representation
    std::vector<torch::jit::IValue> inputs;
    // variable for loading file
    std::ifstream infile;
    // ---- edit: write new get_encoding code for this
    //      should load pcd file for simple environments, may refer to the python loading code
    //      need to modify {1, 16053} to desired size as in python loading code
    #ifdef DEBUG
        std::string pcd_fname_test = "../obs_voxel_test.txt";
        infile.open(pcd_fname_test);

        std::string line_test;
        std::vector<float> tt_test;
        while (getline(infile, line_test)){
            tt_test.push_back(std::atof(line_test.c_str()));
        }
        torch::Tensor torch_tensor_test = torch::from_blob(tt_test.data(), {1,2,2,2});
        std::cout << "torch_tensor[0,0,0,0] = " << torch_tensor_test[0][0][0][0] << "\n";
        std::cout << "torch_tensor[0,0,0,1] = " << torch_tensor_test[0][0][0][1] << "\n";
        std::cout << "torch_tensor[0,0,1,0] = " << torch_tensor_test[0][0][1][0] << "\n";
        std::cout << "torch_tensor[0,0,1,1] = " << torch_tensor_test[0][0][1][1] << "\n";
        std::cout << "torch_tensor[0,1,0,0] = " << torch_tensor_test[0][1][0][0] << "\n";
        std::cout << "torch_tensor[0,1,0,1] = " << torch_tensor_test[0][1][0][1] << "\n";
        std::cout << "torch_tensor[0,1,1,0] = " << torch_tensor_test[0][1][1][0] << "\n";
        std::cout << "torch_tensor[0,1,1,1] = " << torch_tensor_test[0][1][1][1] << "\n";
    #endif


    std::string pcd_fname = "../obs_voxel.txt";
    std::cout << "PCD file: " << pcd_fname << "\n\n\n";
    infile.open(pcd_fname);

    std::string line;
    std::vector<float> tt;
    while (getline(infile, line)){
        tt.push_back(std::atof(line.c_str()));
    }
    torch::Tensor torch_tensor = torch::from_blob(tt.data(), {1,32,32,32});
    inputs.push_back(torch_tensor);
    obs_enc = encoder->forward(inputs).toTensor();
}

MPNetPlanner::~MPNetPlanner()
{
    freeMemory();
    encoder.reset();
    MLP.reset();
}

void MPNetPlanner::clear()
{
    Planner::clear();
    sampler_.reset();
    freeMemory();
    if (nn_)
        nn_->clear();
    lastGoalMotion_ = nullptr;
}

void MPNetPlanner::setup()
{
    Planner::setup();
    tools::SelfConfig sc(si_, getName());
    sc.configurePlannerRange(maxDistance_);

    if (!nn_)
        nn_.reset(tools::SelfConfig::getDefaultNearestNeighbors<Motion *>(this));
    nn_->setDistanceFunction([this](const Motion *a, const Motion *b) { return distanceFunction(a, b); });
}

void MPNetPlanner::freeMemory()
{
    if (nn_)
    {
        std::vector<Motion *> motions;
        nn_->list(motions);
        for (auto &motion : motions)
        {
            if (motion->state != nullptr)
                si_->freeState(motion->state);
            delete motion;
        }
    }
}

StatePtrVec MPNetPlanner::neural_replan(StatePtrVec path, int max_length)
/**
* replan the entire path by checking each segment, if not connectable
* do local plan
**/
{
    StatePtrVec new_path;
    for (int i = 0; i < path.size()-1; i++){
        if (si_->isValid(path[i])){
            new_path.push_back(path[i]);
        }
    }
    new_path.push_back(path.back());

    StatePtrVec res_path;
    res_path.push_back(path[0]);
    for (int i=0; i < new_path.size()-1; i++)
    {
        // check each segment of the path if it is connectable
        bool connected = si_->checkMotion(new_path[i], new_path[i+1]);
        if (!connected)
        {
            // if not, use MPNet to do local replanning
            StatePtrVec minipath = neural_replanner(new_path[i], new_path[i+1], max_length);
            for (int j=1; j < minipath.size(); j++)
            {
                res_path.push_back(minipath[j]);
            }
        }
        else
        {
            // otherwise, just add the next node
            res_path.push_back(new_path[i+1]);
        }
    }
    return res_path;
}

StatePtrVec MPNetPlanner::neural_replanner(base::State* start, base::State* goal, int max_length)
/**
* Given the start and goal of a segment that is not connectable,
* use MPNet to bidirectionally generate a path connecting them, and append new nodes
* in the linked list
**/
{
    int iter = 0;
    int tree = 0;
    StatePtrVec start_tree;
    start_tree.push_back(start);
    StatePtrVec goal_tree;
    goal_tree.push_back(goal);
    StatePtrVec minipath;  // store the result
    base::State* temp = si_->allocState(); // free by freeState(temp)
    bool connected = false;
    while (iter < max_length)
    {
        // start planning tree
        if (tree==0)
        {
            // use the last node of start tree to try to connect to the first node of goal tree
            mpnet_predict(start, goal, temp);
            // check if the new state is in collision, if not, create a new motion in the start tree
            if (si_->isValid(temp))
            {
                base::State* state = si_->allocState();
                si_->copyState(state, temp);
                start_tree.push_back(state);
                start = state;
            }
            tree = 1;
        }

        // goal planning tree
        if (tree==1)
        {
            // use the first node of goal tree to try to connect to the last node of goal tree
            mpnet_predict(goal, start, temp);
            // check if the new state is in collision, if not, create a new motion in the start tree
            if (si_->isValid(temp))
            {
                base::State* state = si_->allocState();
                si_->copyState(state, temp);
                goal_tree.push_back(state);
                goal = state;
            }
            tree = 0;
        }
        // check if start and goal can connect, if so, return the path with connected entire path
        connected = si_->checkMotion(start, goal);
        if (connected)
        {
            break;
        }
        iter ++;
    }
    si_->freeState(temp);
    if (!connected)
    {
        // remove intermediate states, and connect start and goal
        for (int i=1; i < start_tree.size(); i++)
        {
            si_->freeState(start_tree[i]);
        }
        for (int i=1; i < goal_tree.size(); i++)
        {
            si_->freeState(goal_tree[i]);
        }
        minipath.push_back(start_tree[0]);
        minipath.push_back(goal_tree[0]);

        return minipath;
    }
    else
    {
        for (int i=0; i < start_tree.size(); i++)
        {
            minipath.push_back(start_tree[i]);
        }
        for (int i=goal_tree.size()-1; i>-1; i--)
        {
            minipath.push_back(goal_tree[i]);
        }
        return minipath;
    }
}

StatePtrVec MPNetPlanner::lvc(StatePtrVec path)
{
    for (int i=0; i < path.size()-1; i++)
    {
        for (int j=path.size()-1; j>i+1; j--)
        {
            bool ind = 0;
            ind = si_->checkMotion(path[i], path[j]);

            #ifdef DEBUG
                std::cout << "i: " << i << ", j: " << j << " ind: " << ind << "\n";
                std::cout << "path length: " << path.size() << "\n";
            #endif

            if (ind)
            {
                #ifdef DEBUG
                    std::cout << "calling LSC again... \n";
                #endif

                StatePtrVec pc;
                for (size_t k = 0; k < i+1; k++){
                    pc.push_back(path[k]);
                }
                for (size_t k = j; k < path.size(); k++){
                    pc.push_back(path[k]);
                }
                return lvc(pc);
            }
        }
    }
    return path;
}



std::vector<float> MPNetPlanner::normalize(std::vector<float> state, int dim)
{
    // normalize in the 3D state space first
    std::vector<float> res;
    for (int i=0; i<3; i++)
    {
        res.push_back((state[i]-lower_bound[i])/bound[i]-1.0);
    }
    // normalize the quarternion
    float norm = 0.;
    for (int i=3; i<7; i++)
    {
        norm += state[i]*state[i];
    }
    norm = sqrt(norm);
    for (int i=3; i<7; i+++)
    {
        res.push_back(state[i]/norm);
    }
    return res;
}

std::vector<float> MPNetPlanner::unnormalize(std::vector<float> state, int dim)
{
    // normalize in the 3D state space first
    std::vector<float> res;
    for (int i=0; i<3; i++)
    {
        res.push_back((state[i]+1.0)*bound[i]+lower_bound[i]);
    }
    // normalize the quarternion
    float norm = 0.;
    for (int i=3; i<7; i++)
    {
        norm += state[i]*state[i];
    }
    norm = sqrt(norm);
    for (int i=3; i<7; i+++)
    {
        res.push_back(state[i]/norm);
    }
    return res;
}


void MPNetPlanner::mpnet_predict(const base::State* start, const base::State* goal, base::State* next)
{
    // given the start and goal, and the internal obstacle representation
    // convert them to torch::Tensor, and feed into MPNet
    // return the next state to the "next" parameter
    int dim = si_->getStateDimension();
    // get start, goal in tensor form
    torch::Tensor sg = getStartGoalTensor(start, goal, dim);
    //torch::Tensor gs = getStartGoalTensor(goal, start, dim);

    torch::Tensor mlp_input_tensor;
    // Note the order of the cat
    mlp_input_tensor = torch::cat({obs_enc,sg}, 1).to(at::kCUDA);
    auto mlp_output = MLP->forward(mlp_input_tensor);
    torch::Tensor res = mlp_output.toTensor().to(at::kCPU);

    auto res_a = res.accessor<float,2>(); // accesor for the tensor

    std::vector<float> state_vec;
    for (int i = 0; i < dim; i++)
    {
        state_vec.push_back(res_a[0][i]);
    }
    std::vector<float> unnoramlzied_state_vec = unnormalize(state_vec, dim);
    for (int i = 0; i < dim; i++)
    {
        //TODO: better assign by using angleAxis
        next->as<base::RealVectorStateSpace::StateType>()->values[i] = res_a[0][i];
    }
}
torch::Tensor MPNetPlanner::getStartGoalTensor(const base::State *start_state, const base::State *goal_state, int dim){
    //convert to torch tensor by getting data from states
    std::vector<float> goal_vec;
    std::vector<float> start_vec;

    for (int i = 0; i < dim; i++){
        goal_vec.push_back((float)goal_state->as<base::RealVectorStateSpace::StateType>()->values[i]);
        start_vec.push_back((float)start_state->as<base::RealVectorStateSpace::StateType>()->values[i]);
    }


    std::vector<float> normalized_start_vec = normalize(start_vec, dim);
    std::vector<float> normalized_goal_vec = normalize(goal_vec, dim);
    torch::Tensor start_tensor = torch::from_blob(normalized_start_vec.data(), {1, dim});
    torch::Tensor goal_tensor = torch::from_blob(normalized_goal_vec.data(), {1, dim});

    #ifdef DEBUG
        std::cout << "Start Vec: \n" << start_vec << "\n";
        std::cout << "Goal Vec: \n" << goal_vec << "\n";



        std::cout << "Start Vec: " << start_vec << "\n"
                << "Start Tensor: " << start_tensor << "\n"
                << "Goal Vec: " << goal_vec << "\n"
                << "Goal Tensor: " << goal_tensor << "\n";
    #endif

    torch::Tensor sg_cat;
    sg_cat = torch::cat({start_tensor, goal_tensor}, 1);


    #ifdef DEBUG
        std::cout << "\n\n\nCONCATENATED START/GOAL\n\n\n" << sg_cat << "\n\n\n";
    #endif

    return sg_cat;
}


base::PlannerStatus MPNetPlanner::solve(const base::PlannerTerminationCondition &ptc)
{
    checkValidity();
    base::Goal *goal = pdef_->getGoal().get();
    auto *goal_s = dynamic_cast<base::GoalSampleableRegion *>(goal);

    while (const base::State *st = pis_.nextStart())
    {
        auto *motion = new Motion(si_);
        si_->copyState(motion->state, st);
        nn_->add(motion);
    }

    if (nn_->size() == 0)
    {
        OMPL_ERROR("%s: There are no valid initial states!", getName().c_str());
        return base::PlannerStatus::INVALID_START;
    }

    if (!sampler_)
        sampler_ = si_->allocStateSampler();

    OMPL_INFORM("%s: Starting planning with %u states already in datastructure", getName().c_str(), nn_->size());

    Motion *solution = nullptr;
    Motion *approxsol = nullptr;
    double approxdif = std::numeric_limits<double>::infinity();
    auto *rmotion = new Motion(si_);
    base::State *rstate = rmotion->state;
    base::State *xstate = si_->allocState();


    // initialize the path
    const base::State *const_goal_state = goal->as<base::GoalStates>()->getState(0);
    base::State *goal_state = si_->allocState();
    si_->copyState(goal_state, const_goal_state);
    StatePtrVec path;
    path.push_back(pdef_->getStartState(0));
    path.push_back(goal_state);

    // reference to python planning methods
    int iter = 0;
    int max_length = _max_length;

    bool feasible = true;
    while (!ptc)
    {
        if (iter==0)
        {
            max_length = _max_length;
        }
        else if (iter<0.30*_max_replan)
        {
            max_length = _max_length*2;
        }
        else
        {
            max_length = _max_length*3;
        }

        // use neural replan to plan path
        path = neural_replan(path, max_length);

        // collision check for the entire path to see if it is feasible
        feasible = true;

        for (int i=0; i<path.size()-1; i++)
        {
            if (!si_->checkMotion(path[i], path[i+1]))
            {
                feasible = false;
                break;
            }
        }
        if (feasible)
        {
            break;
        }
        iter += 1;
        if (iter > _max_replan)
        {
            break;
        }
    }

    bool solved = false;
    bool approximate = true;
    if (feasible)
    {
        solved = true;
        approximate = false;
    }
    /* set the solution path */
    auto sol_path(std::make_shared<ompl::geometric::PathGeometric>(si_));
    for (int i=0; i<path.size(); i++)
        sol_path->append(path[i]);
    //TODO: modify approxdif to be the approximate difference to real solution
    pdef_->addSolutionPath(sol_path, approximate, approxdif, getName());
    solved = true;

    si_->freeState(xstate);
    if (rmotion->state != nullptr)
        si_->freeState(rmotion->state);
    delete rmotion;

    //OMPL_INFORM("%s: Created %u states", getName().c_str(), nn_->size());
    // remove the planned path
    for (int i=0; i<path.size(); i++)
    {
        si_->freeState(path[i]);
    }
    return {solved, approximate};
}

void MPNetPlanner::getPlannerData(base::PlannerData &data) const
{
    Planner::getPlannerData(data);

    std::vector<Motion *> motions;
    if (nn_)
        nn_->list(motions);

    if (lastGoalMotion_ != nullptr)
        data.addGoalVertex(base::PlannerDataVertex(lastGoalMotion_->state));

    for (auto &motion : motions)
    {
        if (motion->parent == nullptr)
            data.addStartVertex(base::PlannerDataVertex(motion->state));
        else
            data.addEdge(base::PlannerDataVertex(motion->parent->state), base::PlannerDataVertex(motion->state));
    }
}
