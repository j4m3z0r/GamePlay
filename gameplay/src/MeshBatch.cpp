#include "Base.h"
#include "MeshBatch.h"

namespace gameplay
{

MeshBatch::MeshBatch(const VertexFormat& vertexFormat, Mesh::PrimitiveType primitiveType, Material* material, bool indexed, unsigned int initialCapacity, unsigned int growSize)
    : _vertexFormat(vertexFormat), _primitiveType(primitiveType), _material(material), _indexed(indexed), _capacity(0), _growSize(growSize),
      _vertexCapacity(0), _indexCapacity(0), _vertexCount(0), _indexCount(0), _vertices(NULL), _verticesPtr(NULL), _indices(NULL), _indicesPtr(NULL)
#ifdef EMSCRIPTEN
      , _vertexDataObject(0), _vertexDataObjectSize(0), _indexDataObject(0), _indexDataObjectSize(0)
#endif // EMSCRIPTEN
{
    resize(initialCapacity);
}

MeshBatch::~MeshBatch()
{
    SAFE_RELEASE(_material);
    SAFE_DELETE_ARRAY(_vertices);
    SAFE_DELETE_ARRAY(_indices);
}

MeshBatch* MeshBatch::create(const VertexFormat& vertexFormat, Mesh::PrimitiveType primitiveType, const char* materialPath, bool indexed, unsigned int initialCapacity, unsigned int growSize)
{
    Material* material = Material::create(materialPath);
    if (material == NULL)
    {
        GP_ERROR("Failed to create material for mesh batch from file '%s'.", materialPath);
        return NULL;
    }
    MeshBatch* batch = create(vertexFormat, primitiveType, material, indexed, initialCapacity, growSize);
    SAFE_RELEASE(material); // batch now owns the material
    return batch;
}

MeshBatch* MeshBatch::create(const VertexFormat& vertexFormat, Mesh::PrimitiveType primitiveType, Material* material, bool indexed, unsigned int initialCapacity, unsigned int growSize)
{
    GP_ASSERT(material);

    MeshBatch* batch = new MeshBatch(vertexFormat, primitiveType, material, indexed, initialCapacity, growSize);

    material->addRef();

    return batch;
}

void MeshBatch::updateVertexAttributeBinding()
{
#ifdef EMSCRIPTEN
    // We re-allocate the buffer if the new data-set is larger than was
    // previously allocated. Otherwise, we just overwrite the old data.
    GLuint voSize = _vertexFormat.getVertexSize() * _vertexCapacity;
    if(voSize > _vertexDataObjectSize)
    {
        _vertexDataObjectSize = voSize;
        if(_vertexDataObject)
        {
            GL_ASSERT( glDeleteBuffers(1, &_vertexDataObject) );
        }
        GL_ASSERT( glGenBuffers(1, &_vertexDataObject) );
    }
    GL_ASSERT( glBindBuffer(GL_ARRAY_BUFFER, _vertexDataObject) );
    GL_ASSERT( glBufferData(GL_ARRAY_BUFFER, voSize, _vertices, GL_STATIC_DRAW) );
#endif // EMSCRIPTEN

    GP_ASSERT(_material);

    // Update our vertex attribute bindings.
    for (unsigned int i = 0, techniqueCount = _material->getTechniqueCount(); i < techniqueCount; ++i)
    {
        Technique* t = _material->getTechniqueByIndex(i);
        GP_ASSERT(t);
        for (unsigned int j = 0, passCount = t->getPassCount(); j < passCount; ++j)
        {
            Pass* p = t->getPassByIndex(j);
            GP_ASSERT(p);
            VertexAttributeBinding* b = VertexAttributeBinding::create(_vertexFormat, _vertices, p->getEffect());
            p->setVertexAttributeBinding(b);
            SAFE_RELEASE(b);
        }
    }
}

unsigned int MeshBatch::getCapacity() const
{
    return _capacity;
}

void MeshBatch::setCapacity(unsigned int capacity)
{
    resize(capacity);
}

bool MeshBatch::resize(unsigned int capacity)
{
    if (capacity == 0)
    {
        GP_ERROR("Invalid resize capacity (0).");
        return false;
    }

    if (capacity == _capacity)
        return true;

    // Store old batch data.
    unsigned char* oldVertices = _vertices;
    unsigned short* oldIndices = _indices;

    unsigned int vertexCapacity = 0;
    switch (_primitiveType)
    {
    case Mesh::LINES:
        vertexCapacity = capacity * 2;
        break;
    case Mesh::LINE_STRIP:
        vertexCapacity = capacity + 1;
        break;
    case Mesh::POINTS:
        vertexCapacity = capacity;
        break;
    case Mesh::TRIANGLES:
        vertexCapacity = capacity * 3;
        break;
    case Mesh::TRIANGLE_STRIP:
        vertexCapacity = capacity + 2;
        break;
    default:
        GP_ERROR("Unsupported primitive type for mesh batch (%d).", _primitiveType);
        return false;
    }

    // We have no way of knowing how many vertices will be stored in the batch
    // (we only know how many indices will be stored). Assume the worst case
    // for now, which is the same number of vertices as indices.
    unsigned int indexCapacity = vertexCapacity;
    if (_indexed && indexCapacity > USHRT_MAX)
    {
        GP_ERROR("Index capacity is greater than the maximum unsigned short value (%d > %d).", indexCapacity, USHRT_MAX);
        return false;
    }

    // Allocate new data and reset pointers.
    unsigned int voffset = _verticesPtr - _vertices;
    unsigned int vBytes = vertexCapacity * _vertexFormat.getVertexSize();
    _vertices = new unsigned char[vBytes];
    if (voffset >= vBytes)
        voffset = vBytes - 1;
    _verticesPtr = _vertices + voffset;

    if (_indexed)
    {
        unsigned int ioffset = _indicesPtr - _indices;
        _indices = new unsigned short[indexCapacity];
        if (ioffset >= indexCapacity)
            ioffset = indexCapacity - 1;
        _indicesPtr = _indices + ioffset;
    }

    // Copy old data back in
    if (oldVertices)
        memcpy(_vertices, oldVertices, std::min(_vertexCapacity, vertexCapacity) * _vertexFormat.getVertexSize());
    SAFE_DELETE_ARRAY(oldVertices);
    if (oldIndices)
        memcpy(_indices, oldIndices, std::min(_indexCapacity, indexCapacity) * sizeof(unsigned short));
    SAFE_DELETE_ARRAY(oldIndices);

    // Assign new capacities
    _capacity = capacity;
    _vertexCapacity = vertexCapacity;
    _indexCapacity = indexCapacity;

    // Update our vertex attribute bindings now that our client array pointers have changed
#ifndef EMSCRIPTEN
    // WebGL needs complete uploaded buffers to bind attributes to, so we defer
    // this until the ::draw call, when we know the buffers will be complete.
    updateVertexAttributeBinding();
#endif // EMSCRIPTEN

    return true;
}
    
void MeshBatch::start()
{
    _vertexCount = 0;
    _indexCount = 0;
    _verticesPtr = _vertices;
    _indicesPtr = _indices;
}

void MeshBatch::finish()
{
}

void MeshBatch::draw()
{
    if (_vertexCount == 0 || (_indexed && _indexCount == 0))
        return; // nothing to draw

    // Not using VBOs, so unbind the element array buffer.
    // ARRAY_BUFFER will be unbound automatically during pass->bind().
#ifndef EMSCRIPTEN
    GL_ASSERT( glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0 ) );
#endif // EMSCRIPTEN

#ifdef EMSCRIPTEN
    updateVertexAttributeBinding();
#endif // EMSCRIPTEN

    GP_ASSERT(_material);
    if (_indexed)
        GP_ASSERT(_indices);

    // Bind the material.
    Technique* technique = _material->getTechnique();
    GP_ASSERT(technique);


    unsigned int passCount = technique->getPassCount();
    for (unsigned int i = 0; i < passCount; ++i)
    {
        Pass* pass = technique->getPassByIndex(i);
        GP_ASSERT(pass);
        pass->bind();

        if (_indexed)
        {
#ifdef EMSCRIPTEN
            GLuint ioSize = _indexCount * 2;
            if(ioSize > _indexDataObjectSize)
            {
                _indexDataObjectSize = ioSize;
                if(_indexDataObject)
                {
                    GL_ASSERT( glDeleteBuffers(1, &_indexDataObject) );
                }
                GL_ASSERT( glGenBuffers(1, &_indexDataObject) );
            }
            GL_ASSERT( glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, _indexDataObject) );
            GL_ASSERT( glBufferData( GL_ELEMENT_ARRAY_BUFFER, ioSize, _indices, GL_STATIC_DRAW) );

            GL_ASSERT( glDrawElements(_primitiveType, _indexCount, GL_UNSIGNED_SHORT, (GLvoid*)0) );
#else
            GL_ASSERT( glDrawElements(_primitiveType, _indexCount, GL_UNSIGNED_SHORT, (GLvoid*)_indices) );
#endif // EMSCRIPTEN
        }
        else
        {
            GL_ASSERT( glDrawArrays(_primitiveType, 0, _vertexCount) );
        }

        pass->unbind();
    }
}
    

}
