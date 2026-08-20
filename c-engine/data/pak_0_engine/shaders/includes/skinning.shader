/*
 * GPU Skinning
 *
 * Requires globalset.shader to be included first.
 * The including shader must declare these in its push constants:
 *   uint64_t jointMatrixBufferAddress;
 *   uint64_t entitySkinMapBufferAddress;
 *   uint64_t prevJointMatrixBufferAddress;
 */

JointMatrixBuffer jointMatrixBuffer     = JointMatrixBuffer(jointMatrixBufferAddress);
JointMatrixBuffer prevJointMatrixBuffer = JointMatrixBuffer(prevJointMatrixBufferAddress);
EntitySkinMapBuffer entitySkinMap       = EntitySkinMapBuffer(entitySkinMapBufferAddress);

mat4 computeSkinMatrixFromWords(uint skinOffset, uint jointsWord, uint weightsWord) {
    // Unpack 4 joint indices (uint8 each)
    uvec4 joints = uvec4(
        (jointsWord >>  0) & 0xFF,
        (jointsWord >>  8) & 0xFF,
        (jointsWord >> 16) & 0xFF,
        (jointsWord >> 24) & 0xFF
    );

    // Unpack 4 weights (unorm8 each)
    vec4 weights = vec4(
        float((weightsWord >>  0) & 0xFF) / 255.0,
        float((weightsWord >>  8) & 0xFF) / 255.0,
        float((weightsWord >> 16) & 0xFF) / 255.0,
        float((weightsWord >> 24) & 0xFF) / 255.0
    );

    // Compute blended skin matrix
    mat4 skinMatrix = weights.x * jointMatrixBuffer.matrices[skinOffset + joints.x]
                    + weights.y * jointMatrixBuffer.matrices[skinOffset + joints.y]
                    + weights.z * jointMatrixBuffer.matrices[skinOffset + joints.z]
                    + weights.w * jointMatrixBuffer.matrices[skinOffset + joints.w];

    return skinMatrix;
}

mat4 computePrevSkinMatrixFromWords(uint skinOffset, uint jointsWord, uint weightsWord) {
    uvec4 joints = uvec4(
        (jointsWord >>  0) & 0xFF,
        (jointsWord >>  8) & 0xFF,
        (jointsWord >> 16) & 0xFF,
        (jointsWord >> 24) & 0xFF
    );

    vec4 weights = vec4(
        float((weightsWord >>  0) & 0xFF) / 255.0,
        float((weightsWord >>  8) & 0xFF) / 255.0,
        float((weightsWord >> 16) & 0xFF) / 255.0,
        float((weightsWord >> 24) & 0xFF) / 255.0
    );

    mat4 skinMatrix = weights.x * prevJointMatrixBuffer.matrices[skinOffset + joints.x]
                    + weights.y * prevJointMatrixBuffer.matrices[skinOffset + joints.y]
                    + weights.z * prevJointMatrixBuffer.matrices[skinOffset + joints.z]
                    + weights.w * prevJointMatrixBuffer.matrices[skinOffset + joints.w];

    return skinMatrix;
}
