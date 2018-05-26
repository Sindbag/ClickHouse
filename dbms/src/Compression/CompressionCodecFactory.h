#pragma once

#include <memory>
#include <functional>
#include <unordered_map>
#include <ext/singleton.h>
#include <IO/ReadBuffer.h>
#include <Parsers/parseQuery.h>
#include <Parsers/ParserCreateQuery.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTLiteral.h>
#include <Common/typeid_cast.h>
#include <Poco/String.h>
#include <Compression/CompressionPipeline.h>


namespace DB
{

class ICompressionCodec;
using CodecPtr = std::shared_ptr<ICompressionCodec>;

class IAST;
using ASTPtr = std::shared_ptr<IAST>;


/** Creates a codec object by name of compression algorithm family and parameters, also creates codecs pipe.
  */
class CompressionCodecFactory : public ext::singleton<CompressionCodecFactory>
{
private:
    using Creator = std::function<CodecPtr(const ASTPtr & parameters)>;
    using SimpleCreator = std::function<CodecPtr()>;
    using CodecsDictionary = std::unordered_map<String, Creator>;
    using ByteCodecsDictionary = std::unordered_map<char, SimpleCreator>;

public:
    CodecPtr get(const String & full_name) const;
    CodecPtr get(const String & family_name, const ASTPtr & parameters) const;
    CodecPtr get(const ASTPtr & ast) const;
    CodecPtr get(char&) const;
    CodecPtr get_pipe(String & full_declaration);
    CodecPtr get_pipe(ReadBuffer * header);
    CodecPtr get_pipe(ASTPtr& header);

    /// Register a codec family by its name.
    void registerCodec(const String & family_name, Creator creator);

    /// Register a simple codec, that have no parameters.
    void registerSimpleCodec(const String & name, SimpleCreator creator);

    /// Register a codec by its bytecode, it could not have parameters.
    void registerCodecBytecode(const char& bytecode, SimpleCreator creator);

private:
    CodecsDictionary codecs;
    ByteCodecsDictionary bytecodes_codecs;

    CompressionCodecFactory();
    friend class ext::singleton<CompressionCodecFactory>;
};

}
