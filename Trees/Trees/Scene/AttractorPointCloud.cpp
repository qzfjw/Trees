#include "Globals.h"
#include "AttractorPointCloud.h"

void AttractorPointCloud::GeneratePoints(unsigned int numPoints) {
    #ifdef ENABLE_DEBUG_OUTPUT
    auto start = std::chrono::system_clock::now();
    #endif
    for (unsigned int i = 0; i < numPoints; ++i) {
        const glm::vec3 p = glm::vec3(dis(rng) * 2.0f, dis(rng) * 2.0f, dis(rng) * 4.0f);
        points.emplace_back(AttractorPoint(points[i]));
    }
    #ifdef ENABLE_DEBUG_OUTPUT
    auto end = std::chrono::system_clock::now();
    std::chrono::duration<double> elapsed_seconds = end - start;
    std::time_t end_time = std::chrono::system_clock::to_time_t(end);
    std::cout << "Elapsed time for Attractor Point Cloud Generation: " << elapsed_seconds.count() << "s\n";
    std::cout << "Number of Attractor Points Generated: " << points.size() << "\n\n";
    #endif
    create();
}

void AttractorPointCloud::GeneratePoints(const Mesh& m, unsigned int numPoints) {
    #ifdef ENABLE_DEBUG_OUTPUT
    auto start = std::chrono::system_clock::now();
    #endif
    for (unsigned int i = 0; i < numPoints; ++i) {
        const glm::vec3 p = glm::vec3(dis(rng) * 2.0f, dis(rng) * 2.0f, dis(rng) * 4.0f);

        // Intersect with mesh
        if (m.Contains(p)) {
            points.emplace_back(AttractorPoint(p));
        }
    }
    #ifdef ENABLE_DEBUG_OUTPUT
    auto end = std::chrono::system_clock::now();
    std::chrono::duration<double> elapsed_seconds = end - start;
    std::time_t end_time = std::chrono::system_clock::to_time_t(end);
    std::cout << "Elapsed time for Attractor Point Cloud Generation: " << elapsed_seconds.count() << "s\n";
    std::cout << "Number of Attractor Points Generated: " << points.size() << "\n\n";
    #endif
    create();
}

void AttractorPointCloud::create() {
    // Indices
    genBufIdx();

    // Create an indices vector
    std::vector<unsigned int> indices = std::vector<unsigned int>();
    for (int i = 0; i < points.size(); ++i) {
        indices.emplace_back(i);
    }

    count = (int)indices.size();
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bufIdx);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(unsigned int) * indices.size(), indices.data(), GL_STATIC_DRAW);

    // Positions
    genBufPos();
    glBindBuffer(GL_ARRAY_BUFFER, bufPos);
    glBufferData(GL_ARRAY_BUFFER, sizeof(glm::vec3) * points.size(), points.data(), GL_STATIC_DRAW);
}
