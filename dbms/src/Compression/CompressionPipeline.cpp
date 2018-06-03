#include <IO/ReadBuffer.h>
#include <IO/CompressedStream.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTExpressionList.h>
#include <Parsers/ParserCreateQuery.h>
#include <Parsers/parseQuery.h>
#include <Common/typeid_cast.h>
#include <Compression/CompressionPipeline.h>
#include <Compression/CompressionCodecFactory.h>
#include <DataTypes/IDataType.h>


namespace DB
{

class IAST;
using ASTPtr = std::shared_ptr<IAST>;
using ASTs = std::vector<ASTPtr>;

CompressionPipeline::CompressionPipeline(ReadBuffer * header)
{
    const CompressionCodecFactory & codec_factory = CompressionCodecFactory::instance();
    char last_codec_bytecode, last_bytecode;
    PODArray<char> _header;
    header_size = 0;
    /// Read codecs, while continuation bit is set
    do {
        _header.resize(1);
        header->readStrict(&_header[0], 1);
        header_size += 1;

        last_bytecode = _header[0];
        last_codec_bytecode = last_bytecode & ~static_cast<uint8_t>(CompressionMethodByte::CONTINUATION_BIT);
        auto _codec = codec_factory.get(last_codec_bytecode);

        if (_codec->getHeaderSize())
        {
            _header.resize(_codec->getHeaderSize());
            header->readStrict(&_header[0], _codec->getHeaderSize());
            header_size += _codec->parseHeader(&_header[0]);
        }
        codecs.push_back(_codec);
    }
    while (last_bytecode & static_cast<uint8_t>(CompressionMethodByte::CONTINUATION_BIT));
    /// Load and reverse sizes part of a header, listed from later codecs to the original size, - see `compress`.
    auto codecs_amount = codecs.size();
    data_sizes.resize(codecs_amount + 1);

    header_size += sizeof(UInt32) * (codecs_amount + 1);
    _header.resize(sizeof(UInt32) * (codecs_amount + 1));
    header->readStrict(&_header[0], sizeof(UInt32) * (codecs_amount + 1));

    for (size_t i = 0; i <= codecs_amount; ++i)
        data_sizes[codecs_amount - i] = unalignedLoad<UInt32>(&_header[sizeof(UInt32) * i]);
    data_sizes[codecs_amount] -= getHeaderSize(); /// remove header size from last data size

    if (header_size != getHeaderSize())
        throw("Incorrect header read size: " + std::to_string(header_size) + ", expected " +
              std::to_string(getHeaderSize()), ErrorCodes::LOGICAL_ERROR);
}

CompressionPipelinePtr CompressionPipeline::createPipelineFromBuffer(ReadBuffer * header)
{
    return std::make_shared<CompressionPipeline>(header);
}

CompressionPipelinePtr CompressionPipeline::createPipelineFromASTPtr(ASTPtr & ast_codec)
{
    Codecs codecs;
    ASTs & args_func = typeid_cast<ASTFunction &>(*ast_codec).children;

    if (args_func.size() != 1)
        throw Exception("Codecs pipeline definition must have parameters.", ErrorCodes::LOGICAL_ERROR);

    ASTs & ast_codecs = typeid_cast<ASTExpressionList &>(*args_func.at(0)).children;
    for (auto & codec : ast_codecs)
        codecs.emplace_back(std::move(CompressionCodecFactory::instance().get(codec)));

    /// AST codecs needed for declaration copy in formatColumns function
    auto shared_obj = std::make_shared<CompressionPipeline>(codecs);
    shared_obj->codec_ptr = ast_codec;

    return shared_obj;
}

CompressionPipelinePtr CompressionPipeline::createPipelineFromString(const String & full_declaration)
{
    Codecs codecs;
    ParserCodecDeclarationList codecs_parser;
    ASTPtr ast = parseQuery(codecs_parser, full_declaration.data(), full_declaration.data() + full_declaration.size(), "codecs", 0);
    ASTs & ast_codecs = typeid_cast<ASTExpressionList &>(*ast).children;

    for (auto & codec : ast_codecs)
        codecs.emplace_back(std::move(CompressionCodecFactory::instance().get(codec)));

    return std::make_shared<CompressionPipeline>(codecs);
}

String CompressionPipeline::getName() const
{
    String name("CODEC(");
    for (size_t i = 0; i < codecs.size(); ++i)
        name += !i ? codecs[i]->getName() : ", " + codecs[i]->getName();
    name += ")";
    return name;
}

const char * CompressionPipeline::getFamilyName() const
{
    return "CODEC";
}

size_t CompressionPipeline::getCompressedSize() const
{
    return data_sizes.back();
}

size_t CompressionPipeline::getDecompressedSize() const
{
    return data_sizes.front();
}

size_t CompressionPipeline::writeHeader(char * out, std::vector<uint32_t> & ds)
{
    size_t wrote_size = 0;
    for (size_t i = 0; i < codecs.size(); ++i)
    {
        auto wrote = codecs[i]->writeHeader(out);
        if (i != codecs.size() - 1)
            *out |= static_cast<uint8_t>(CompressionMethodByte::CONTINUATION_BIT);

        out += wrote;
        wrote_size += wrote;
    }

    uint32_t codecs_amount = codecs.size();
    for (int32_t i = codecs_amount; i >= 0; --i)
    {
        if (i == static_cast<int32_t>(codecs_amount))
        {
            uint32_t compressed_size = ds[i] + getHeaderSize();
            unalignedStore(&out[sizeof(uint32_t) * (codecs_amount - i)], const_cast<uint32_t &>(compressed_size));
        }
        else
        {
            unalignedStore(&out[sizeof(uint32_t) * (codecs_amount - i)], ds[i]);
        }
    }

    wrote_size += sizeof(uint32_t) * (codecs_amount + 1);
    return wrote_size;
}

size_t CompressionPipeline::getHeaderSize() const
{
    /// Header size as sum of codecs headers' sizes
    size_t _hs = sizeof(UInt32); /// decompressed size
    for (auto &codec : codecs)
        ///    bytecode  + arguments part + data size part
        _hs += 1 + codec->getHeaderSize() + sizeof(UInt32);
    return _hs;
}

size_t CompressionPipeline::getMaxCompressedSize(size_t uncompressed_size) const
{
    return codecs[0]->getMaxCompressedSize(uncompressed_size);
}

size_t CompressionPipeline::compress(char * source, PODArray<char> & dest, size_t input_size, size_t max_output_size)
{
    std::vector<uint32_t> ds {static_cast<uint32_t>(input_size)};
    ds.resize(codecs.size() + 1);

    char * _source = source;
    auto * _dest = &dest;
    auto hs = getHeaderSize();
    PODArray<char> buffer1;

    for (size_t i = 0; i < codecs.size(); ++i)
    {
        (*_dest).resize(hs + max_output_size);
        input_size = codecs[i]->compress(_source, &(*_dest)[hs], input_size, max_output_size);
        ds[i + 1] = input_size;

        _source = &(*_dest)[hs];
        _dest = _dest == &dest ? &buffer1 : &dest;
        max_output_size = i + 1 < codecs.size() ? codecs[i + 1]->getMaxCompressedSize(input_size) : input_size;
    }

    if (_dest == &dest)
    {
        dest.resize(hs + input_size);
        memcpy(&dest[hs], &buffer1[hs], input_size);
    }
    /// Write header data
    size_t header_wrote_size = writeHeader(&dest[0], ds);

    if (hs != header_wrote_size)
        throw Exception("Bad header formatting", ErrorCodes::LOGICAL_ERROR);

    return input_size + header_wrote_size;
}

size_t CompressionPipeline::decompress(char *source, char *dest, size_t input_size, size_t output_size)
{
    PODArray<char> buffer1, buffer2;
    char * _source = source;
    auto * _dest = &buffer1;
    size_t mid_size = 0;

    for (int i = codecs.size() - 1; i >= 0; --i) {
        mid_size = data_sizes[i];
        if (!i) /// output would be dest
        {
            input_size = codecs[i]->decompress(_source, dest, input_size, mid_size);
        }
        else
        {
            (*_dest).resize(mid_size);
            input_size = codecs[i]->decompress(_source, &(*_dest)[0], input_size, mid_size);
            _source = &(*_dest)[0];
            _dest = _dest == &buffer1 ? &buffer2 : &buffer1;
        }
    }

    if (input_size != output_size)
        throw Exception("Decoding problem: got " + std::to_string(input_size) + " instead of " + std::to_string(output_size),
                        ErrorCodes::LOGICAL_ERROR);

    return input_size;
}

void CompressionPipeline::setDataType(DataTypePtr data_type_)
{
    data_type = data_type_;
    for (auto & codec: codecs)
        codec->setDataType(data_type);
}

std::vector<UInt32> CompressionPipeline::getDataSizes() const
{
    auto ds(data_sizes);
    return ds;
}

};