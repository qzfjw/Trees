#include "glad/glad.h"
#include "GLFW/glfw3.h"
#include "glm/glm.hpp"
#include "glm/gtx/norm.hpp"
#include "pcg_random.hpp"

#include "OpenGL\ShaderProgram.h"
#include "Scene\Mesh.h"
#include "Scene\Camera.h"

#include <iostream>
#include <vector>
#include <random>

// For performance analysis / timing
//#include <chrono>
//#include <ctime>

#define GLM_FORCE_RADIANS
#define VIEWPORT_WIDTH_INITIAL 800
#define VIEWPORT_HEIGHT_INITIAL 600

// draw branches as 3d geometry vs gl lines for branches
//#define CUBES

// Tweak dis
#define INITIAL_BRANCH_RADIUS 0.1f
#define BUD_OCCUPANCY_RADIUS 0.1f
#define BUD_PERCEPTION_RADIUS 0.2f // should be a multiple of the internode length of the branch that the bud is on. Not a define

// Used in space colonization
#define COS_THETA 0.70710678118f

// For BH Model
#define ALPHA 1.0f // proportionality constant for resource flow computation
#define LAMBDA 0.5f

// For addition of new shoots
#define GROWTH_DIR_WEIGHT 0.5f
#define TROPISM_DIR_WEIGHT 0.5f

// For 5-tree scene, eye and ref: glm::vec3(0.25f, 0.5f, 3.5f), glm::vec3(0.25f, 0.0f, 0.0f
Camera camera = Camera(glm::vec3(0.125f, 0.4f, 0.325f), glm::vec3(0.0f, 0.4f, 0.0f), 0.7853981634f, // 45 degrees vs 75 degrees
                          (float)VIEWPORT_WIDTH_INITIAL / VIEWPORT_HEIGHT_INITIAL, 0.01f, 10.0f);

void framebuffer_size_callback(GLFWwindow* window, int width, int height) {
    glViewport(0, 0, width, height);
    camera.SetAspect((float)width / height);
}

void processInput(GLFWwindow *window) {
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
        glfwSetWindowShouldClose(window, true);
}

class AttractorPoint {
private:
    glm::vec3 point; // Point in world space
    float killDist; // Radius for removal

public:
    AttractorPoint(const glm::vec3& p, const float& d) : point(p), killDist(d), nearestBudDist2(9999999.0f), nearestBudBranchIdx(-1), nearestBudIdx(-1) {}
    ~AttractorPoint() {}
    inline const glm::vec3 GetPoint() const {
        return point;
    }
    inline bool IsKilledBy(const glm::vec3& p) const {
        return glm::distance2(p, point) < (killDist * killDist);
    }
    // Make these private
    // Newer paper variables
    float nearestBudDist2; // how close the nearest bud is that has this point in its perception volume, squared
    int nearestBudBranchIdx; // index in the array of the branch of that bud ^^
    int nearestBudIdx; // index in the array the bud of that branch ^^
    // For the indices: -1 indicates, not yet set, -2 indicates the terminal bud of the branch
};

/// New proposed code structure for tree growth simulation

enum BUD_FATE {
    DORMANT,
    FORMED_BRANCH,
    FORMED_FLOWER,
    ABORT // TODO: when does this happen
};

enum BUD_TYPE {
    TERMINAL, // at the end of a branch
    LATERAL // along the sides of a branch
};

// Store any information relevant to a particular bud
struct Bud {
    glm::vec3 point;
    glm::vec3 branchGrowthDir; // Growth direction of this bud. Use Golden Angle (137.5 degrees) for lateral buds.
    glm::vec3 optimalGrowthDir; // optimal growth direction computing during space colonization
    float occupancyRadius; // Radius about the bud in which attractor points are removed
    float environmentQuality; // In space colonization, this is a binary 0 or 1
    float accumEnvironmentQuality; // Using Borchert-Honda Model, indicates the accumulated amount of resources reaching this bud
    float resourceBH; // amount of available resource reaching this Bud using the BH Model
    int formedBranchIndex; // If this bud's fate is FORMED_BRANCH, this is the index in the Tree's list of branches of that formed branch. -1 o.w.
    float internodeLength;
    BUD_TYPE type;
    BUD_FATE fate;
    
    // Constructor: to allow use with emplace_back() on vectors
    Bud(const glm::vec3& p, const glm::vec3& b, const glm::vec3& d, const float r, const float q, const float aq, const float re,
        const int i, const float l, BUD_TYPE t, BUD_FATE f) :
        point(p), branchGrowthDir(b), optimalGrowthDir(d), occupancyRadius(r), environmentQuality(q), accumEnvironmentQuality(aq), resourceBH(re),
        formedBranchIndex(i), internodeLength(l), type(t), fate(f) {}
};

// Wraps up necessary information regarding a tree branch.
class TreeBranch {
    friend class Tree;
private: // TODO: make the terminal bud just be the last bud in the one list of buds. Too complicated differentiating it in the code.
    std::vector<Bud> buds; // List of buds. Last bud is always the terminal bud.
    glm::vec3 growthDirection; // World space direction in which this branch is oriented
    float radius; // Branch radius. Computed using pipe model
    unsigned int axisOrder; // Order n (0, 1, ..., n) of this axis. Original trunk of a tree is 0, each branch supported by this branch has order 1, etc
    int prevBranchIndex; // Index of the branch supporting this one in the 

public:
    TreeBranch(const glm::vec3& p, const int ao) :
        growthDirection(glm::vec3(0.0f, 1.0f, 0.0f)), radius(INITIAL_BRANCH_RADIUS), axisOrder(ao) {
        buds = std::vector<Bud>();
        buds.reserve(4); // Reserve memory beforehand so we are less likely to have to resize the array later on. Performance test this.
        buds.emplace_back(p, glm::vec3(growthDirection), glm::vec3(0.0f), BUD_OCCUPANCY_RADIUS, 0.0f, 0.0f, 0.0f, -1, 0.0f, TERMINAL, DORMANT); // add the terminal bud for this branch
        // maybe make the reserve value a function of the iterations, later iterations will probably be shorter than an early branch that has
        // been around for awhile?
    }
    // Adds a certain number of axillary buds to the list of buds, starting at the index just before the terminal bud
    void AddAxillaryBuds(const int numBuds, const float internodeLength) {
        // Create a temporary list of Buds that will be inserted in this branch's list of buds
        std::vector<Bud> newBuds = std::vector<Bud>();
        
        // Axillary Buds will all make an angle of 137.5 degrees (the Golden Angle) with the direction of the given branch.
        const glm::quat branchQuat = glm::angleAxis(glm::radians(137.5f), glm::normalize(glm::cross(growthDirection, WORLD_UP_VECTOR)));
        const glm::mat4 budRotMat = glm::toMat4(branchQuat);
        glm::vec3 budGrowthDir = glm::normalize(glm::vec3(budRotMat * glm::vec4(growthDirection, 0.0f)));

        // buds will be inserted @ current terminal bud pos + (float)b * budGrowthDir * internodeLength
        Bud& terminalBud = buds[buds.size() - 1]; // last bud is always the terminal bud
        for (int b = 0; b < numBuds; ++b) {
            newBuds.emplace_back(terminalBud.point + (float)b * budGrowthDir * internodeLength, budGrowthDir, glm::vec3(0.0f),
                                   BUD_OCCUPANCY_RADIUS, 0.0f, 0.0f, 0.0f, -1, 0.0f, LATERAL, DORMANT);
        }
        // Update terminal bud position 
        terminalBud.point = terminalBud.point + (float)(newBuds.size()) * budGrowthDir * internodeLength;
        buds.insert(buds.end() - 1, newBuds.begin(), newBuds.end()); // this might crash if only the terminal bud is in the list, not sure
    }
};

// Wrap up branches into one Tree class. This class organizes the simulation functions
class Tree {
private:
    std::vector<TreeBranch> branches;
public:
    Tree(const glm::vec3& p) {
        branches.reserve(256); // Reserve a lot so we don't have to resize often. This vector will definitely expand a lot.
        InitializeTree(p);
    }
    void InitializeTree(const glm::vec3& p) { branches.emplace_back(TreeBranch(p, 0)); } // Init a tree to be a single branch
    void IterateGrowth(const int numIters, std::vector<AttractorPoint>& attractorPoints) {
        for (int n = 0; n < numIters; ++n) {
            IterateSpaceColonization(attractorPoints); // 1. Compute Q (presence of space/light) and optimal growth direction using space colonization
            ComputeBHModel();                          // 2. Using BH Model, flow resource basipetally and then acropetally
            AppendNewShoots();                         // 3. Add new shoots using the resource computed in previous step
        }
        

        
    }
    void IterateSpaceColonization(std::vector<AttractorPoint>& attractorPoints) {
        // 1. Remove all attractor points that are too close to any bud
        for (int br = 0; br < branches.size(); ++br) {
            const std::vector<Bud>& buds = branches[br].buds;
            for (int bu = 0; bu < buds.size(); ++bu) {
                auto attrPtIter = attractorPoints.begin();
                while (attrPtIter != attractorPoints.end()) {
                    const Bud& currentBud = buds[bu];
                    const float budToPtDist = glm::length2(attrPtIter->GetPoint() - currentBud.point);
                    if (budToPtDist < currentBud.occupancyRadius * currentBud.occupancyRadius) {
                        attrPtIter = attractorPoints.erase(attrPtIter); // This attractor point is close to the bud, remove it
                    } else {
                        ++attrPtIter;
                    }
                }
            }
        }

        // 2. Compute the optimal growth direction for each bud
        for (int br = 0; br < branches.size(); ++br) {
            std::vector<Bud>& buds = branches[br].buds;
            const int numBuds = buds.size();
            for (int bu = 0; bu < numBuds; ++bu) {
                auto attrPtIter = attractorPoints.begin();
                while (attrPtIter != attractorPoints.end()) { // if a bud is FORMED_BRANCH, do i not run the alg on it? probs just test, but need to add geometry correctly
                    Bud& currentBud = buds[bu];
                    if (currentBud.fate == DORMANT) { // early measure, aka buds wont grow after branching once. TODO also keep in mind that branching points store buds twice.
                        const glm::vec3 budToPtDir = attrPtIter->GetPoint() - currentBud.point; // Use current lateral or terminal bud
                        const float dotProd = glm::dot(glm::normalize(budToPtDir), currentBud.branchGrowthDir); // TODO: This is only true for terminal buds. Lateral buds should use the "golden angle"
                        const float budToPtDist2 = glm::length2(budToPtDir);
                        if (budToPtDist2 < BUD_PERCEPTION_RADIUS * BUD_PERCEPTION_RADIUS && dotProd > std::abs(COS_THETA)) {
                            // Any given attractor point can only be perceived by one bud - the nearest one.
                            // If we end up find a bud closer to this attractor point than the previously recorded one,
                            // update the point accordingly and remove this attractor point's contribution from that bud's
                            // growth direction vector.
                            if (budToPtDist2 < attrPtIter->nearestBudDist2) {
                                attrPtIter->nearestBudDist2 = budToPtDist2;
                                if (attrPtIter->nearestBudBranchIdx != -1) {
                                    if (attrPtIter->nearestBudIdx != -1) {
                                        branches[attrPtIter->nearestBudBranchIdx].buds[attrPtIter->nearestBudIdx].optimalGrowthDir -= budToPtDir;
                                    }
                                }
                                attrPtIter->nearestBudBranchIdx = br;
                                attrPtIter->nearestBudIdx = bu;
                                currentBud.optimalGrowthDir += budToPtDir;
                            }

                        }
                        ++attrPtIter;
                    }
                }
            }
        }
    }
    void ComputeBHModel() { // Perform each pass of the BH Model for resource flow
        IterateBHModelBasipetalPass();
        IterateBHModelAcropetalPass();
    }

    // make this a non-member helper function in the cpp file
    float ComputeQAccumRecursive(TreeBranch& branch) {
        float accumQ = 0.0f;
        for (int bu = 0; bu < branch.buds.size(); ++bu) {
            Bud& currentBud = branch.buds[bu];
            switch (currentBud.type) {
            case TERMINAL:
                accumQ += currentBud.environmentQuality;
                break;
            case LATERAL:
                // Need to check whether this lateral bud has actually formed a branch
                switch (currentBud.fate) {
                case DORMANT:
                    accumQ += currentBud.environmentQuality;
                    break;
                case FORMED_BRANCH:
                    accumQ += ComputeQAccumRecursive(branches[currentBud.formedBranchIndex]);
                    break;
                case FORMED_FLOWER: // double check if we include the resource in this case TODO
                    accumQ += ComputeQAccumRecursive(branches[currentBud.formedBranchIndex]);
                    break;
                default: // includes ABORT case - ignore this bud
                    break;
                }
                break;
            }
            currentBud.accumEnvironmentQuality = accumQ;
            return accumQ;
        }
    }
    void ComputeBHModelBasipetalPass() { // Compute the amount of resource reaching each internode (actually stored in bud above that internode)
        // Way 1:
        // Make a recursive function that takes in a particular branch, it should return the incoming Q at the base of the branch.
        // Should just be a for loop with one recursive call.
        // This is a little inefficient and I should eventually memoize the information so we don't have to recompute branches.
        ComputeQAccumRecursive(branches[0]); // ignore return value
    }

    // make this a non-member helper function in the cpp file
    void ComputeResourceFlowRecursive(TreeBranch& branch, float resource) {
        for (int bu = 0; bu < branch.buds.size(); ++bu) {
            Bud& currentBud = branch.buds[bu];
            // this all only applies to lateral buds. if terminal, just set the amount of resource
            // if this bud is dormant, set this bud's amount of resource to whatever the current value is
            // if this bud is formed flower or abort, set the amount of resource to 0
            // else, aka if this bud is formed branch, then set the resource amount for this branch to the main axis part of the
            // formula, and make an additional call on the branch at the appropriate index with the other portion of the eqn
            // in the formula: v is resource, Qm is the accumQ stored in buds[bu+1]. Ql is accumQ stored branches[currentBud.formedBranchIndex].buds[1].
            // Yes that is a hard coded index but there should be an invariant that any lateral bud that has no consecutive lateral buds is followed by
            // a terminal bud.
            // Note, deciding now that at branching points, a bud will be stored in both branches. It's a little more memory intensive, and I might
            // change this later, but right now it simplifies things a bit.
            switch (currentBud.type) {
            case TERMINAL:
                currentBud.resourceBH = resource;
                break;
            case LATERAL:
                switch (currentBud.fate) {
                case DORMANT:
                    currentBud.resourceBH = resource;
                    break;
                case FORMED_BRANCH: { // have to scope for nontrivial cases, apparently: https://stackoverflow.com/questions/10381144/error-c2361-initialization-of-found-is-skipped-by-default-label
                    TreeBranch& lateralBranch = branches[currentBud.formedBranchIndex];
                    const float Qm = branch.buds[bu + 1].accumEnvironmentQuality; // Q on main axis
                    const float Ql = lateralBranch.buds[1].accumEnvironmentQuality; // Q on lateral axis
                    const float denom = LAMBDA * Qm + (1.0f - LAMBDA) * Ql;
                    currentBud.resourceBH = resource * (LAMBDA * Qm) / denom; // formula for main axis
                    ComputeResourceFlowRecursive(lateralBranch, resource * (1.0f - LAMBDA) * Ql / denom); // Call this function on the lateral branch with the other formula
                    resource = currentBud.resourceBH; // resource reaching the remaining buds in this branch have the attenuated resource
                    break;
                }
                case FORMED_FLOWER:
                    currentBud.resourceBH = 0.0f;
                    break;
                default: // FORMED_FLOWER or ABORT
                    currentBud.resourceBH = 0.0f;
                    break;

                }
                break;
            }
        }
    }
    void ComputeBHModelAcropetalPass() { // Recursive like basipetal pass, but will definitely need to memoize or something
        // pass in the first branch and the base amount of resource (v)
        ComputeResourceFlowRecursive(branches[0], branches[0].buds[branches[0].buds.size() - 1].accumEnvironmentQuality * ALPHA); //probably have to pass in the base v value or something idk rn im tired
    }

    void AppendNewShoots() {
        // for each branch, for each bud, compute floor(v). if that's > 0, check if its a terminal bud. if yes, just extend the current axis.
        // if its a lateral bud, do the hard invariant stuff.
        // need to compute the new growth axis. use golden angle for lateral buds
        for (int br = 0; br < branches.size(); ++br) {
            std::vector<Bud>& buds = branches[br].buds;
            for (int bu = 0; bu < buds.size(); ++bu) {
                Bud& currentBud = buds[bu];
                const int numMetamers = std::floor(currentBud.resourceBH);
                if (numMetamers > 0) {
                    const float metamerLength = currentBud.resourceBH / (float)numMetamers;
                    for (int m = 1; m <= numMetamers; ++m) { // append new shoots/branches in the direction of the bud
                        // this is going to move memory entirely by copying which is lame but fix later TODO
                        TreeBranch newBranch = TreeBranch(currentBud.point, branches[br].axisOrder + 1);
                    }
                }
            }
        }
    }
};

// Wrap the overall application up into a TreeApplication class eventually that controls both GL calls and Tree objects
/*class TreeApplication {

};*/

// Tree growth code - will need to be refactored TO ANOTHER FILE PLS ALSO CANT TURN OFF CAPS LOCK RN SO
// this is now outdated **

class TreeNode {
private:
    glm::vec3 point; // Point in world space
    float influenceDist; // Radius of sphere of influence
    int parentIdx; // index of the parent of this Node, in the array of nodes. Will probably change
    std::vector<int> childrenIndices; // vector containing the indices of each child. Each child should have this node as its parent

public:
    TreeNode(const glm::vec3& p, const float& d, const int& i, const int& it) :
        point(p), influenceDist(d), parentIdx(i), optimalGrowthDir(glm::vec3(0.0f)), branchDir(glm::vec3(0.0f)), hasNearbyAttrPts(false), iter(it) {
        childrenIndices = std::vector<int>();
        childrenIndices.reserve(10); // reserve 10 children...this can be tweaked and should be performance analyzed
    }
    ~TreeNode() {}
    inline bool InfluencesPoint(const glm::vec3& p) const {
        return glm::distance2(p, point) < (influenceDist * influenceDist);
    }
    inline const glm::vec3 GetPoint() const {
        return point;
    }
    inline const int GetParentIndex() const {
        return parentIdx;
    }
    // Variables, I'm being lazy, make them private
    // Newer paper parameters
    glm::vec3 optimalGrowthDir;
    glm::vec3 branchDir;
    bool hasNearbyAttrPts;
    unsigned int iter; // what iteration this node was added during
};

int main() {
    // Test Mesh Loading
    //Mesh m = Mesh();
    //m.LoadFromFile("OBJs/plane.obj");

    // GLFW Window Setup
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(VIEWPORT_WIDTH_INITIAL, VIEWPORT_HEIGHT_INITIAL, "Trees", NULL, NULL);
    if (window == NULL) {
        std::cout << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return -1;
    }
    glfwMakeContextCurrent(window);

    // Initialize Glad
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cout << "Failed to initialize GLAD" << std::endl;
        return -1;
    }

    glViewport(0, 0, VIEWPORT_WIDTH_INITIAL, VIEWPORT_HEIGHT_INITIAL);
    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);

    // Test loop vectorization. Note: using the compiler flags, this stuff only seems to compile in Release Mode
    // Needed flags: /O2 /Qvec-report:1 (can also use report:2)
    // Source: https://software.intel.com/en-us/articles/a-guide-to-auto-vectorization-with-intel-c-compilers

    // Stores the point positions: currently a list of floats. I need to include glm or eigen
    // Is it faster to initialize a vector of points with # and value and then set the values, or to push_back values onto an empty list
    // Answer to that: https://stackoverflow.com/questions/32199388/what-is-better-reserve-vector-capacity-preallocate-to-size-or-push-back-in-loo
    // Best options seem to be preallocate or emplace_back with reserve
    const unsigned int numPoints = 500000;
    unsigned int numPointsIncluded = 0;
    std::vector<glm::vec3> points = std::vector<glm::vec3>();

    // Using PCG RNG: http://www.pcg-random.org/using-pcg-cpp.html

    // Make a random number engine
    pcg32 rng(101);

    std::uniform_real_distribution<float> dis(-1.0f, 1.0f);

    // Create points
    // Unfortunately, we can't really do any memory preallocating because we don't actually know how many points will be included
    for (unsigned int i = 0; i < numPoints; ++i) {
        const glm::vec3 p = glm::vec3(dis(rng), dis(rng), dis(rng));
        if ((p.x * p.x + p.y * p.y + p.z * p.z) < 0.015f /*p.y > 0.2f*/ /*&& (p.x * p.x + p.y * p.y) > 0.2f*/) {
            points.emplace_back(p + glm::vec3(0.0f, 0.45f, 0.0f));
            ++numPointsIncluded;
        }
    }

    // Create the AttractorPoints
    const float killDist = 0.05f;
    std::vector<AttractorPoint> attractorPoints = std::vector<AttractorPoint>();
    attractorPoints.reserve(numPointsIncluded);
    for (unsigned int i = 0; i < numPointsIncluded; ++i) {
        attractorPoints.emplace_back(AttractorPoint(points[i], killDist));
    }

    // Create the TreeNode(s)
    const float branchLength = 0.03f;
    const float branchInflDist = branchLength * 2.75f; // according to second paper, should be 2 * internode-length
    std::vector<TreeNode> treeNodes = std::vector<TreeNode>();

    treeNodes.emplace_back(TreeNode(glm::vec3(0.01f, 0.28f, 0.0f), branchInflDist, -1, 0));
    //treeNodes.emplace_back(TreeNode(glm::vec3(0.1f, 0.38f, 0.0f), branchInflDist, 0, 0));

    // Run the tree algorithm
    //TODO
    //for a certain number of iterations?
    //for each tree node
    // for each point
    //    get each point in reach. add it to a vector of points.
    //       compute the direction of the next treenode, create it a distance of d away
    //for each point
    //  for each tree node
    //    if a tree node is in the kill distance, remove this attractor point

    const unsigned int numIters = 2;
    unsigned int numTreeNodes = (unsigned int)treeNodes.size();

    //#define DO_ALG

    #ifdef DO_ALG
    #define MODIFIED_SPACE_COL

    #ifndef MODIFIED_SPACE_COL // original space col algorithm

    // For numTreeNodes: Update the number of tree nodes to run the algorithm on in the for loop
    bool didUpdate = false; // Flag indicating that none of the tree nodes had any nearby points, so kill the algorithm
    
    for (unsigned int n = 0; n < numIters && attractorPoints.size() > 0; ++n) {
        // Create TreeNodes
        for (unsigned int ti = 0; ti < numTreeNodes; ++ti) { // Perform the algorithm for each tree node
            glm::vec3 accumDir = glm::vec3(0.0f); // Accumulate the direction of each influencing AttractorPoint
            bool numNearbyPoints = false; // Flag indicating there is at least one nearby attractor point
            // rename that ^^^ lol it's 'num'
            const TreeNode& currTreeNode = treeNodes[ti];
            const glm::vec3& treeNodePoint = currTreeNode.GetPoint();

            for (unsigned int pi = 0; pi < attractorPoints.size(); ++pi) {
                const glm::vec3& attrPoint = attractorPoints[pi].GetPoint();

                if (currTreeNode.InfluencesPoint(attrPoint)) {
                    accumDir += attrPoint - treeNodePoint;
                    numNearbyPoints = true;
                }
            }

            // If at least one attractor point is within the sphere of influence of this tree node
            if (numNearbyPoints) {
                didUpdate = true;
                // Normalize the accumulated direction
                accumDir = glm::normalize(accumDir);

                /*const glm::vec3 tropism = glm::vec3(0.0f, -0.01f * n * n, 0.0f);
                accumDir += tropism;*/
                //accumDir = glm::normalize(accumDir);

                // Create a new TreeNode
                treeNodes.emplace_back(TreeNode(treeNodePoint + accumDir * branchLength, branchInflDist, ti, n));
            }
        }

        if (!didUpdate) {
            break; // kill the algorithm if we don't update any tree nodes
        }

        // Kill attractor points that need to be killed
        // https://stackoverflow.com/questions/347441/erasing-elements-from-a-vector

        auto attrPtIter = attractorPoints.begin();

        //int i = 0; // count where we are in the loop for when we break for debugging reasons
        while (attrPtIter != attractorPoints.end()) {
            bool didRemovePoint = false;
            for (unsigned int ti = 0; ti < numTreeNodes; ++ti) { // size does NOT include the newly created tree nodes
                if (attrPtIter->IsKilledBy(treeNodes[ti].GetPoint())) {
                    attrPtIter = attractorPoints.erase(attrPtIter);
                    didRemovePoint = true;
                    break;
                }
            }
            if (!didRemovePoint) {
                ++attrPtIter;
            }
        }
        //std::cout << "Num points left: " << attractorPoints.size() << std::endl;
        numTreeNodes = (unsigned int)treeNodes.size();

        /*for (unsigned int pi = 0; pi < attractorPoints.size(); ++pi) {
            const AttractorPoint& currAttrPt = attractorPoints[pi];
            for (unsigned int ti = 0; ti < treeNodes.size(); ++ti) { // size includes the newly created tree nodes
                if (currAttrPt.IsKilledBy(treeNodes[ti].GetPoint())) {
                    attractorPoints.erase(attractorPoints.begin() + pi);
                    // TODO: update the buffers that get drawn
                }
            }
        }*/
    }

    // Print out info
    std::cout << "Number of Iterations: " << numIters << std::endl;
    std::cout << "Branch Length: " << branchLength << std::endl;
    std::cout << "Kill Distance: " << killDist << std::endl;
    std::cout << "Node Influence Distance: " << branchInflDist << std::endl;
    std::cout << "Number of attractor points (initial): " << numPointsIncluded << std::endl;
    std::cout << "Number of attractor points (final): " << attractorPoints.size() << std::endl;
    std::cout << "Number of Tree Nodes: " << treeNodes.size() << std::endl;

    #else // do modified space col algorithm

    for (int n = 0; n < numIters && attractorPoints.size() > 0; ++n) {
        // 1. Remove all points that lie within the occupancy sphere of each tree node
        // (Based on the above original space col algorithm)
        // https://stackoverflow.com/questions/347441/erasing-elements-from-a-vector

        //int i = 0;
        auto attrPtIter = attractorPoints.begin();
        while (attrPtIter != attractorPoints.end()) {
            auto endIter = attractorPoints.end();
            auto beginIter = attractorPoints.begin();
            bool didRemovePoint = false;
            for (unsigned int ti = 0; ti < numTreeNodes; ++ti) {
                if (treeNodes[ti].InfluencesPoint(attrPtIter->GetPoint())) {
                    attrPtIter = attractorPoints.erase(attrPtIter);
                    didRemovePoint = true;
                    break;
                }
            }
            if (!didRemovePoint) {
                ++attrPtIter;
            }
            //++i;
        }

        // 2. Get all attractor points within the perception volume
        // Use hard-coded parameter values in the paper:
        // theta = 90 degrees
        // occupancy sphere radius is set to 2 internode lengths already
        // perception radius = 4 - 6 internode lengths

        const float cosTheta = std::cos(0.78539816339f); // cos(pi/4), can just hard code this of course
        const float perceptionRad = branchLength * 4.0f;

        // have to store the starting amount of tree nodes so the vector doesn't extend and the loop goes infinitely
        for (unsigned int ti = 0; ti < numTreeNodes; ++ti) {
            glm::vec3 accumDir = glm::vec3(0.0f); // Accumulate the direction of each influencing AttractorPoint
            TreeNode& currNode = treeNodes[ti];
            const glm::vec3& currNodePoint = currNode.GetPoint();
            const unsigned int parentIdx = currNode.GetParentIndex();
            glm::vec3 branchDir;
            if(parentIdx == -1) {
                branchDir = glm::vec3(0.0f, 1.0f, 0.0f); // what to do for very first node? TODO ***
            } else {
                const TreeNode& currBaseNode = treeNodes[currNode.GetParentIndex()];
                branchDir = glm::normalize(currNodePoint - currBaseNode.GetPoint());
            }
            
            currNode.branchDir = branchDir;

            bool numNearbyPoints = false; // Flag indicating there is at least one nearby attractor point

            //int count = 0;
            for (int pi = 0; pi < attractorPoints.size(); ++pi) {
                AttractorPoint& attrPtRef = attractorPoints[pi];
                const glm::vec3& attrPt = attrPtRef.GetPoint();
                const glm::vec3 attrPtDir = attrPt - currNodePoint;
                const float dotProd = dot(branchDir, glm::normalize(attrPtDir)); // for debuggin only *** delete me TODO
                if (length(attrPtDir) < perceptionRad && dotProd > std::abs(cosTheta)) {
                    //count++;
                    numNearbyPoints = true;
                    accumDir += attrPtDir;
                    if (length(attrPtDir) < attrPtRef.nearestDist) { // optimize to use length2() *** TODO and also store in a register
                        // This is messy but keep for now:
                        if (attrPtRef.nearestTreeNodeIdx != -1 && attrPtRef.nearestTreeNodeIdx != ti) {
                            // each time we find out a node is closer, remove this point's contribution to that node's growth direction
                            treeNodes[attrPtRef.nearestTreeNodeIdx].optimalGrowthDir -= attrPtDir;
                        }
                        attrPtRef.nearestDist = length(accumDir);
                        attrPtRef.nearestTreeNodeIdx = ti;
                    }
                }
            }

            if (numNearbyPoints) {
                //accumDir = glm::normalize(accumDir);

                // Note: this isn't how this algorithm should run. It should actually just store the optimal growth direction.
                // The second stage of the bud growth permeates the available "resources" throughout the tree, which determines the growth of shoots
                //treeNodes.emplace_back(TreeNode(currNodePoint + accumDir * branchLength, branchInflDist, ti));

                // Do the tree growth in second stage kinda
                currNode.optimalGrowthDir = accumDir;
                currNode.hasNearbyAttrPts = true; // has > 0 nearby attr pts
            }
        }

        // Now place all the new branches
        // this will evenetually be replaced with the priority model for shoot growth
        // this is not optimized rn ****** TODO
        for (unsigned ti = 0; ti < numTreeNodes; ++ti) {
            if (treeNodes[ti].hasNearbyAttrPts) {
                const glm::vec3 tropism = glm::vec3(0.0f, -1.0f, 0.0f);
                treeNodes.emplace_back(TreeNode(treeNodes[ti].GetPoint() +
                    // actual growth direction
                    glm::normalize(treeNodes[ti].optimalGrowthDir * 2.0f + treeNodes[ti].branchDir + 0.5f * tropism) * branchLength,
                    branchInflDist, ti, n));
            }
        }

        numTreeNodes = (unsigned int)treeNodes.size(); // update total number of tree nodes
    }

    std::cout << "Number of Tree Nodes: " << treeNodes.size() << std::endl;

    #endif

    #endif


    // Code for GL stuff



    // Create indices for the attractor points
    std::vector<unsigned int> indices = std::vector<unsigned int>(numPointsIncluded);
    for (unsigned int i = 0; i < numPointsIncluded; ++i) {
        indices[i] = i;
    }

    // Untransformed cylinder code

    /*std::vector<glm::vec3> cylPoints = std::vector<glm::vec3>();
    std::vector<glm::vec3> cylNormals = std::vector<glm::vec3>();
    std::vector<unsigned int> cylIndices = std::vector<unsigned int>();
    const float radius = 0.0015f;
    // Could speed this up by calling .reserve()
    // should support variable LOD branch cylinders? maybe later TODO

    // Generate Positions: and normals
    // Store top cap verts (Indices 0 - 19)
    for (int i = 0; i < 20; ++i) {
        cylPoints.emplace_back(glm::vec3(glm::rotate(glm::mat4(1.0f), glm::radians(i * 18.0f), glm::vec3(0, 1, 0)) * glm::vec4(radius, 1, 0, 1)));
        cylNormals.emplace_back(glm::vec3(0, 1, 0));
    }

    // Store bottom cap verts (Indices 20 - 39)
    for (int i = 20; i < 40; ++i) {
        cylPoints.emplace_back(glm::vec3(glm::rotate(glm::mat4(1.0f), glm::radians((i - 20) * 18.0f), glm::vec3(0, 1, 0)) * glm::vec4(radius, -1, 0, 1)));
        cylNormals.emplace_back(glm::vec3(0, -1, 0));
    }

    // Generate second rings for the barrel of the cylinder

    // Store top cap verts (Indices 40 - 59)
    for (int i = 0; i < 20; ++i) {
        const glm::mat4 rot = glm::rotate(glm::mat4(1.0f), glm::radians(i * 18.0f), glm::vec3(0, 1, 0));
        cylPoints.emplace_back(glm::vec3(rot * glm::vec4(radius, 1, 0, 1)));
        cylNormals.emplace_back(glm::vec3(rot * glm::vec4(1, 0, 0, 0)));
    }
    // Store bottom cap verts (Indices 60 - 79)
    for (int i = 20; i < 40; ++i) {
        const glm::mat4 rot = glm::rotate(glm::mat4(1.0f), glm::radians((i - 20) * 18.0f), glm::vec3(0, 1, 0));
        cylPoints.emplace_back(glm::vec3(rot * glm::vec4(radius, -1, 0, 1)));
        cylNormals.emplace_back(glm::vec3(rot * glm::vec4(1, 0, 0, 0)));
    }


    // Generate Indices:
    //Build indices for the top cap (18 tris, indices 0 - 53, up to vertex 19)
    for (int i = 0; i < 18; ++i) {
        cylIndices.emplace_back(0);
        cylIndices.emplace_back(i + 1);
        cylIndices.emplace_back(i + 2);
    }
    //Build indices for the top cap (18 tris, indices 54 - 107, up to vertex 39)
    for (int i = 18; i < 36; ++i) {
        cylIndices.emplace_back(20);
        cylIndices.emplace_back(i + 3);
        cylIndices.emplace_back(i + 4);
    }
    //Build indices for the barrel of the cylinder
    for (int i = 0; i < 19; ++i) {
        cylIndices.emplace_back(i + 40);
        cylIndices.emplace_back(i + 41);
        cylIndices.emplace_back(i + 60);
        cylIndices.emplace_back(i + 41);
        cylIndices.emplace_back(i + 61);
        cylIndices.emplace_back(i + 60);
    }
    //Build the last quad of the barrel, which has looping indices
    cylIndices.emplace_back(59);
    cylIndices.emplace_back(40);
    cylIndices.emplace_back(79);
    cylIndices.emplace_back(40);
    cylIndices.emplace_back(60);
    cylIndices.emplace_back(79);*/

    // End Cylinder generation

    // Now using cubes / rectangular prisms, will be slightly faster and have less visual issues

    std::vector<glm::vec3> cubePoints;
    std::vector<glm::vec3> cubeNormals;
    std::vector<unsigned int> cubeIndices;
    const float scale = 0.0015f;

    ///Positions
    //Front face
    cubePoints.emplace_back(glm::vec3(scale, 1.0f, scale));
    cubePoints.emplace_back(glm::vec3(scale, -1.0f, scale));
    cubePoints.emplace_back(glm::vec3(-scale, -1.0f, scale));
    cubePoints.emplace_back(glm::vec3(-scale, 1.0f, scale));
    
    //Right face
    cubePoints.emplace_back(glm::vec3(scale, 1.0f, -scale));
    cubePoints.emplace_back(glm::vec3(scale, -1.0f, -scale));
    cubePoints.emplace_back(glm::vec3(scale, -1.0f, scale));
    cubePoints.emplace_back(glm::vec3(scale, 1.0f, scale));
    
    //Left face
    cubePoints.emplace_back(glm::vec3(-scale, 1.0f, scale));
    cubePoints.emplace_back(glm::vec3(-scale, -1.0f, scale));
    cubePoints.emplace_back(glm::vec3(-scale, -1.0f, -scale));
    cubePoints.emplace_back(glm::vec3(-scale, 1.0f, -scale));
    
    //Back face
    cubePoints.emplace_back(glm::vec3(-scale, 1.0f, -scale));
    cubePoints.emplace_back(glm::vec3(-scale, -1.0f, -scale));
    cubePoints.emplace_back(glm::vec3(scale, -1.0f, -scale));
    cubePoints.emplace_back(glm::vec3(scale, 1.0f, -scale));
    
    //Top face
    cubePoints.emplace_back(glm::vec3(scale, 1.0f, -scale));
    cubePoints.emplace_back(glm::vec3(scale, 1.0f, scale));
    cubePoints.emplace_back(glm::vec3(-scale, 1.0f, scale));
    cubePoints.emplace_back(glm::vec3(-scale, 1.0f, -scale));
    
    //Bottom face
    cubePoints.emplace_back(glm::vec3(scale, -1.0f, scale));
    cubePoints.emplace_back(glm::vec3(scale, -1.0f, -scale));
    cubePoints.emplace_back(glm::vec3(-scale, -1.0f, -scale));
    cubePoints.emplace_back(glm::vec3(-scale, -1.0f, scale));

    /// Normals
    //Front
    for (int i = 0; i < 4; ++i) {
        cubeNormals.emplace_back(glm::vec3(0, 0, 1));
    }
    //Right
    for (int i = 0; i < 4; ++i) {
        cubeNormals.emplace_back(glm::vec3(1, 0, 0));
    }
    //Left
    for (int i = 0; i < 4; ++i) {
        cubeNormals.emplace_back(glm::vec3(-1, 0, 0));
    }
    //Back
    for (int i = 0; i < 4; ++i) {
        cubeNormals.emplace_back(glm::vec3(0, 0, -1));
    }
    //Top
    for (int i = 0; i < 4; ++i) {
        cubeNormals.emplace_back(glm::vec3(0, 1, 0));
    }
    //Bottom
    for (int i = 0; i < 4; ++i) {
        cubeNormals.emplace_back(glm::vec3(0, -1, 0));
    }

    /// Indices
    for (int i = 0; i < 6; i++) {
        cubeIndices.emplace_back(i * 4);
        cubeIndices.emplace_back(i * 4 + 1);
        cubeIndices.emplace_back(i * 4 + 2);
        cubeIndices.emplace_back(i * 4);
        cubeIndices.emplace_back(i * 4 + 2);
        cubeIndices.emplace_back(i * 4 + 3);
    }

    // End cube generation

    // Create points and indices for the tree branches
    std::vector<glm::vec3> pointsTreeBranch = std::vector<glm::vec3>();
    std::vector<glm::vec3> normalsTreeBranch = std::vector<glm::vec3>();
    std::vector<unsigned int> indicesTreeBranch = std::vector<unsigned int>();
    int idxCounter = 0;
    for (unsigned int i = (unsigned int)treeNodes.size() - 1; i > 0; --i) {
        const TreeNode& currTreeNode = treeNodes[i];
        const int parentIdx = currTreeNode.GetParentIndex();
        if (parentIdx != -1) {
            // Copy over the platonic cylinder/cube positions
            // transform them individually i guess
            // Check if i should emplace_back repeatedly or create a whole new vector and use insert - performance compare after getting them working
            // Could/Should move this to a compute shader bc doing this sequentially is lame

            // Compute the rotation using quaternion
            
            #ifdef CUBES
            const glm::vec3 branchBasePoint = treeNodes[parentIdx].GetPoint();
            glm::vec3 branchAxis = currTreeNode.GetPoint() - branchBasePoint;

            // Compute the translation component real quick - set to the the base branch point + 0.5 * D, placing the cylinder at the halfway point
            const glm::vec3 translation = branchBasePoint + 0.5f * branchAxis;
            const float branchAxisLength = length(branchAxis);
            
            // Back to rotation
            branchAxis /= branchAxisLength;
            const glm::vec3 axis = glm::normalize(glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), branchAxis)); // might be negative / TODO *** goes to zero for perfectly vertical trees. find a new way to do this?
            const float angle = std::acos(glm::dot(glm::vec3(0.0f, 1.0f, 0.0f), branchAxis));

            const glm::quat branchQuat = glm::angleAxis(angle, axis);
            glm::mat4 branchTransform = glm::toMat4(branchQuat); // initially just a rotation matrix

            // Create an overall transformation matrix of translation and rotation
            branchTransform = glm::translate(glm::mat4(1.0f), translation) * branchTransform * glm::scale(glm::mat4(1.0f),
                glm::vec3(2.0f * 1.0f / std::powf(1.25f, (float)currTreeNode.iter), branchAxisLength * 0.5f, 2.0f * 1.0f / std::powf(1.25f, (float)currTreeNode.iter)));

            std::vector<glm::vec3> cubePointsTrans = std::vector<glm::vec3>();
            std::vector<glm::vec3> cubeNormalsTrans = std::vector<glm::vec3>();
            // Interleave VBO data
            for (int i = 0; i < cubePoints.size(); ++i) {
                cubePointsTrans.emplace_back(glm::vec3(branchTransform * glm::vec4(cubePoints[i], 1.0f)));
                glm::vec3 transformedNormal = glm::vec3(glm::inverse(glm::transpose(branchTransform)) * glm::vec4(cubeNormals[i], 0.0f));
                cubeNormalsTrans.emplace_back(transformedNormal); // multiply by inverse transpose for normals
            }

            std::vector<unsigned int> cubeIndicesNew = std::vector<unsigned int>();
            for (int i = 0; i < cubeIndices.size(); ++i) {
                const unsigned int size = (unsigned int)pointsTreeBranch.size();
                cubeIndicesNew.emplace_back(cubeIndices[i] + size); // offset this set of indices by the # of positions. Divide by two bc it contains positions and normals
            }

            pointsTreeBranch.insert(pointsTreeBranch.end(), cubePointsTrans.begin(), cubePointsTrans.end());
            normalsTreeBranch.insert(normalsTreeBranch.end(), cubeNormalsTrans.begin(), cubeNormalsTrans.end());
            indicesTreeBranch.insert(indicesTreeBranch.end(), cubeIndicesNew.begin(), cubeIndicesNew.end());
            
            #else

            // Creation for GL_LINES branches
            pointsTreeBranch.push_back(treeNodes[parentIdx].GetPoint()); // base of branch
            pointsTreeBranch.push_back(currTreeNode.GetPoint()); // branch end point
            indicesTreeBranch.emplace_back(idxCounter++);
            indicesTreeBranch.emplace_back(idxCounter++);
            #endif
        }
    }
    
    /// GL calls and drawing
    
    ShaderProgram sp = ShaderProgram("Shaders/point-vert.vert", "Shaders/point-frag.frag");
    ShaderProgram sp2 = ShaderProgram("Shaders/treeNode-vert.vert", "Shaders/treeNode-frag.frag");
    ShaderProgram sp3 = ShaderProgram("Shaders/mesh-vert.vert", "Shaders/mesh-frag.frag");
    
    // Array/Buffer Objects
    unsigned int VAO, VAO2, VAO3;
    unsigned int VBO, VBO2, VBO3;
    unsigned int EBO, EBO2, EBO3;
    glGenVertexArrays(1, &VAO);
    glGenVertexArrays(1, &VAO2);
    glGenVertexArrays(1, &VAO3);
    glGenBuffers(1, &VBO);
    glGenBuffers(1, &VBO2);
    glGenBuffers(1, &VBO3);
    glGenBuffers(1, &EBO);
    glGenBuffers(1, &EBO2);
    glGenBuffers(1, &EBO3);

    // VAO Binding
    glBindVertexArray(VAO);

    // VBO Binding
    // Points
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    std::vector<glm::vec3> tempPts = std::vector<glm::vec3>();
    tempPts.reserve(attractorPoints.size());
    for (int i = 0; i < attractorPoints.size(); ++i) {
        tempPts.emplace_back(attractorPoints[i].GetPoint());
    }
    std::vector<unsigned int> tempPtsIdx = std::vector<unsigned int>();
    tempPtsIdx.reserve(attractorPoints.size());
    for (int i = 0; i < attractorPoints.size(); ++i) {
        tempPtsIdx.emplace_back(i);
    }
    glBufferData(GL_ARRAY_BUFFER, sizeof(glm::vec3) * tempPts.size(), tempPts.data(), GL_STATIC_DRAW);
    // EBO Binding
    // Points
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(unsigned int) * tempPtsIdx.size(), tempPtsIdx.data(), GL_STATIC_DRAW);
    // Attribute linking
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    glBindVertexArray(VAO2);
    glBindBuffer(GL_ARRAY_BUFFER, VBO2);
    #ifdef CUBES
    // Positions
    glBufferData(GL_ARRAY_BUFFER, sizeof(glm::vec3) * pointsTreeBranch.size(), pointsTreeBranch.data(), GL_STATIC_DRAW);
    // Tree Branches EBO
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO2);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(unsigned int) * indicesTreeBranch.size(), indicesTreeBranch.data(), GL_STATIC_DRAW);
    // Attribute linking
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), (void*)0);
    glEnableVertexAttribArray(0);

    // Normals
    glBindBuffer(GL_ARRAY_BUFFER, VBO3);
    glBufferData(GL_ARRAY_BUFFER, sizeof(glm::vec3) * normalsTreeBranch.size(), normalsTreeBranch.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO3);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(unsigned int) * indicesTreeBranch.size(), indicesTreeBranch.data(), GL_STATIC_DRAW);
    // Attribute Linking
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), (void*)0);
    glEnableVertexAttribArray(1);

    #else
    // GL Lines
    glBufferData(GL_ARRAY_BUFFER, sizeof(glm::vec3) * pointsTreeBranch.size(), pointsTreeBranch.data(), GL_STATIC_DRAW);
    // Tree Branches EBO
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO2);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(unsigned int) * indicesTreeBranch.size(), indicesTreeBranch.data(), GL_STATIC_DRAW);
    // Attribute linking
    // Pos
    // Shader uses the normal but just leave it blank here and it'll render black which is ok i guess
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    #endif
    
    
    // This was TinyOBJ Debugging
    /*for (int i = 0; i < m.GetVertices().size(); i++) {
        std::cout << m.GetVertices()[i].pos.x << m.GetVertices()[i].pos.y << m.GetVertices()[i].pos.z << std::endl;
        std::cout << m.GetVertices()[i].nor.x << m.GetVertices()[i].nor.y << m.GetVertices()[i].nor.z << std::endl;
    }

    for (int i = 0; i < m.GetIndices().size(); i++) {
        std::cout << m.GetIndices()[i] << std::endl;
    }*/

    //std::vector<unsigned int> idx = m.GetIndices();

    // Mesh buffers
    /*glBindVertexArray(VAO3);
    glBindBuffer(GL_ARRAY_BUFFER, VBO3);
    glBufferData(GL_ARRAY_BUFFER, sizeof(Vertex) * m.GetVertices().size(), m.GetVertices().data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO3);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(unsigned int) * idx.size(), idx.data(), GL_STATIC_DRAW);
    // Attribute linking

    // Positions + Normals
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);
    glEnableVertexAttribArray(0);
    // Bind the 0th VBO. Set up attribute pointers to location 1 for normals.
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)sizeof(glm::vec3)); // skip the first Vertex.pos
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);*/

    glPointSize(2);
    glLineWidth(1);
    glEnable(GL_DEPTH_TEST); 
    //glDepthFunc(GL_LESS);

    // Render loop
    while (!glfwWindowShouldClose(window)) {
        processInput(window);

        glClearColor(0.1f, 0.2f, 0.3f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glBindVertexArray(VAO);
        sp.setCameraViewProj("cameraViewProj", camera.GetViewProj());
        glDrawElements(GL_POINTS, (GLsizei) tempPtsIdx.size(), GL_UNSIGNED_INT, 0);
        
        glBindVertexArray(VAO2);
        sp2.setCameraViewProj("cameraViewProj", camera.GetViewProj());
        #ifdef CUBES
        glDrawElements(GL_TRIANGLES, (GLsizei)indicesTreeBranch.size(), GL_UNSIGNED_INT, 0);
        #else
        glDrawElements(GL_LINES, (GLsizei)indicesTreeBranch.size(), GL_UNSIGNED_INT, 0);
        sp.use();
        glDrawElements(GL_POINTS, (GLsizei)indicesTreeBranch.size(), GL_UNSIGNED_INT, 0);
        #endif

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    glDeleteVertexArrays(1, &VAO);
    glDeleteVertexArrays(1, &VAO2);
    glDeleteVertexArrays(1, &VAO3);
    glDeleteBuffers(1, &VBO);
    glDeleteBuffers(1, &VBO2);
    glDeleteBuffers(1, &VBO3);
    glDeleteBuffers(1, &EBO);
    glDeleteBuffers(1, &EBO2);
    glDeleteBuffers(1, &EBO3);

    glfwTerminate();
    return 0;
}
