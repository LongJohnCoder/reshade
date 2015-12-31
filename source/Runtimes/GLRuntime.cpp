#include "Log.hpp"
#include "GLRuntime.hpp"
#include "FX\ParserNodes.hpp"
#include "GUI.hpp"
#include "WindowWatcher.hpp"

#include <assert.h>
#include <nanovg_gl.h>
#include <boost\algorithm\string.hpp>

#ifdef _DEBUG
	#define GLCHECK(call) { glGetError(); call; GLenum __e = glGetError(); if (__e != GL_NO_ERROR) { char __m[1024]; sprintf_s(__m, "OpenGL Error %x at line %d: %s", __e, __LINE__, #call); MessageBoxA(nullptr, __m, 0, MB_ICONERROR); } }
#else
	#define GLCHECK(call) call
#endif

#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT 0x8C4D
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT 0x8C4E
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT 0x8C4F
#define GL_COMPRESSED_LUMINANCE_LATC1_EXT 0x8C70
#define GL_COMPRESSED_LUMINANCE_ALPHA_LATC2_EXT 0x8C72

// ---------------------------------------------------------------------------------------------------

namespace ReShade
{
	namespace Runtimes
	{
		struct GLTexture : public Texture
		{
			enum class Source
			{
				None,
				Memory,
				BackBuffer,
				DepthStencil
			};

			GLTexture() : ID(), DataSource(Source::None)
			{
			}
			~GLTexture()
			{
				if (DataSource == Source::Memory)
				{
					GLCHECK(glDeleteTextures(2, ID));
				}
			}

			void ChangeDataSource(Source source, GLuint texture, GLuint textureSRGB)
			{
				if (DataSource == Source::Memory)
				{
					GLCHECK(glDeleteTextures(2, ID));
				}

				DataSource = source;

				if (textureSRGB == 0)
				{
					textureSRGB = texture;
				}

				if (ID[0] == texture && ID[1] == textureSRGB)
				{
					return;
				}

				ID[0] = texture;
				ID[1] = textureSRGB;
			}

			GLuint ID[2];
			Source DataSource;
		};
		struct GLSampler
		{
			GLuint ID;
			GLTexture *Texture;
			bool SRGB;
		};
		struct GLTechnique : public Technique
		{
			struct Pass
			{
				GLuint Program;
				GLuint Framebuffer, DrawTextures[8];
				GLint StencilRef;
				GLuint StencilMask, StencilReadMask;
				GLsizei ViewportWidth, ViewportHeight;
				GLenum DrawBuffers[8], BlendEqColor, BlendEqAlpha, BlendFuncSrc, BlendFuncDest, DepthFunc, StencilFunc, StencilOpFail, StencilOpZFail, StencilOpZPass;
				GLboolean FramebufferSRGB, Blend, DepthMask, DepthTest, StencilTest, ColorMaskR, ColorMaskG, ColorMaskB, ColorMaskA;
			};

			~GLTechnique()
			{
				for (auto &pass : Passes)
				{
					GLCHECK(glDeleteProgram(pass.Program));
					GLCHECK(glDeleteFramebuffers(1, &pass.Framebuffer));
				}
			}

			std::vector<Pass> Passes;
		};

		namespace
		{
			class GLEffectCompiler : private boost::noncopyable
			{
			public:
				GLEffectCompiler(const FX::NodeTree &ast) : _ast(ast), _runtime(nullptr), _isFatal(false), _currentFunction(nullptr), _currentGlobalSize(0)
				{
				}

				bool Compile(GLRuntime *runtime, std::string &errors)
				{
					_runtime = runtime;

					_isFatal = false;
					_errors.clear();

					_globalCode.clear();

					for (auto type : _ast.Structs)
					{
						Visit(_globalCode, static_cast<FX::Nodes::Struct *>(type));
					}

					for (auto uniform1 : _ast.Uniforms)
					{
						const auto uniform = static_cast<FX::Nodes::Variable *>(uniform1);

						if (uniform->Type.IsTexture())
						{
							VisitTexture(uniform);
						}
						else if (uniform->Type.IsSampler())
						{
							VisitSampler(uniform);
						}
						else if (uniform->Type.HasQualifier(FX::Nodes::Type::Qualifier::Uniform))
						{
							VisitUniform(uniform);
						}
						else
						{
							Visit(_globalCode, uniform);

							_globalCode += ";\n";
						}
					}

					for (auto function1 : _ast.Functions)
					{
						const auto function = static_cast<FX::Nodes::Function *>(function1);

						_currentFunction = function;

						Visit(_functions[function].SourceCode, function);
					}

					for (auto technique : _ast.Techniques)
					{
						VisitTechnique(static_cast<FX::Nodes::Technique *>(technique));
					}

					if (_currentGlobalSize != 0)
					{
						glGenBuffers(1, &_runtime->_effectUBO);

						GLint previous = 0;
						glGetIntegerv(GL_UNIFORM_BUFFER_BINDING, &previous);

						glBindBuffer(GL_UNIFORM_BUFFER, _runtime->_effectUBO);
						glBufferData(GL_UNIFORM_BUFFER, _runtime->GetConstantStorageSize(), _runtime->GetConstantStorage(), GL_DYNAMIC_DRAW);
					
						glBindBuffer(GL_UNIFORM_BUFFER, previous);
					}

					errors += _errors;

					return !_isFatal;
				}

				static GLenum LiteralToCompFunc(unsigned int value)
				{
					switch (value)
					{
						default:
						case FX::Nodes::Pass::States::ALWAYS:
							return GL_ALWAYS;
						case FX::Nodes::Pass::States::NEVER:
							return GL_NEVER;
						case FX::Nodes::Pass::States::EQUAL:
							return GL_EQUAL;
						case FX::Nodes::Pass::States::NOTEQUAL:
							return GL_NOTEQUAL;
						case FX::Nodes::Pass::States::LESS:
							return GL_LESS;
						case FX::Nodes::Pass::States::LESSEQUAL:
							return GL_LEQUAL;
						case FX::Nodes::Pass::States::GREATER:
							return GL_GREATER;
						case FX::Nodes::Pass::States::GREATEREQUAL:
							return GL_GEQUAL;
					}
				}
				static GLenum LiteralToBlendEq(unsigned int value)
				{
					switch (value)
					{
						case FX::Nodes::Pass::States::ADD:
							return GL_FUNC_ADD;
						case FX::Nodes::Pass::States::SUBTRACT:
							return GL_FUNC_SUBTRACT;
						case FX::Nodes::Pass::States::REVSUBTRACT:
							return GL_FUNC_REVERSE_SUBTRACT;
						case FX::Nodes::Pass::States::MIN:
							return GL_MIN;
						case FX::Nodes::Pass::States::MAX:
							return GL_MAX;
					}

					return GL_NONE;
				}
				static GLenum LiteralToBlendFunc(unsigned int value)
				{
					switch (value)
					{
						case FX::Nodes::Pass::States::ZERO:
							return GL_ZERO;
						case FX::Nodes::Pass::States::ONE:
							return GL_ONE;
						case FX::Nodes::Pass::States::SRCCOLOR:
							return GL_SRC_COLOR;
						case FX::Nodes::Pass::States::SRCALPHA:
							return GL_SRC_ALPHA;
						case FX::Nodes::Pass::States::INVSRCCOLOR:
							return GL_ONE_MINUS_SRC_COLOR;
						case FX::Nodes::Pass::States::INVSRCALPHA:
							return GL_ONE_MINUS_SRC_ALPHA;
						case FX::Nodes::Pass::States::DESTCOLOR:
							return GL_DST_COLOR;
						case FX::Nodes::Pass::States::DESTALPHA:
							return GL_DST_ALPHA;
						case FX::Nodes::Pass::States::INVDESTCOLOR:
							return GL_ONE_MINUS_DST_COLOR;
						case FX::Nodes::Pass::States::INVDESTALPHA:
							return GL_ONE_MINUS_DST_ALPHA;
					}

					return GL_NONE;
				}
				static GLenum LiteralToStencilOp(unsigned int value)
				{
					switch (value)
					{
						default:
						case FX::Nodes::Pass::States::KEEP:
							return GL_KEEP;
						case FX::Nodes::Pass::States::ZERO:
							return GL_ZERO;
						case FX::Nodes::Pass::States::REPLACE:
							return GL_REPLACE;
						case FX::Nodes::Pass::States::INCR:
							return GL_INCR_WRAP;
						case FX::Nodes::Pass::States::INCRSAT:
							return GL_INCR;
						case FX::Nodes::Pass::States::DECR:
							return GL_DECR_WRAP;
						case FX::Nodes::Pass::States::DECRSAT:
							return GL_DECR;
						case FX::Nodes::Pass::States::INVERT:
							return GL_INVERT;
					}
				}
				static GLenum LiteralToTextureWrap(unsigned int value)
				{
					switch (value)
					{
						case FX::Nodes::Variable::Properties::REPEAT:
							return GL_REPEAT;
						case FX::Nodes::Variable::Properties::MIRROR:
							return GL_MIRRORED_REPEAT;
						case FX::Nodes::Variable::Properties::CLAMP:
							return GL_CLAMP_TO_EDGE;
						case FX::Nodes::Variable::Properties::BORDER:
							return GL_CLAMP_TO_BORDER;
					}

					return GL_NONE;
				}
				static GLenum LiteralToTextureFilter(unsigned int value)
				{
					switch (value)
					{
						case FX::Nodes::Variable::Properties::POINT:
							return GL_NEAREST;
						case FX::Nodes::Variable::Properties::LINEAR:
							return GL_LINEAR;
						case FX::Nodes::Variable::Properties::ANISOTROPIC:
							return GL_LINEAR_MIPMAP_LINEAR;
					}

					return GL_NONE;
				}
				static void LiteralToFormat(unsigned int value, GLenum &internalformat, GLenum &internalformatsrgb, Texture::PixelFormat &name)
				{
					switch (value)
					{
						case FX::Nodes::Variable::Properties::R8:
							name = Texture::PixelFormat::R8;
							internalformat = internalformatsrgb = GL_R8;
							break;
						case FX::Nodes::Variable::Properties::R16F:
							name = Texture::PixelFormat::R16F;
							internalformat = internalformatsrgb = GL_R16F;
							break;
						case FX::Nodes::Variable::Properties::R32F:
							name = Texture::PixelFormat::R32F;
							internalformat = internalformatsrgb = GL_R32F;
							break;
						case FX::Nodes::Variable::Properties::RG8:
							name = Texture::PixelFormat::RG8;
							internalformat = internalformatsrgb = GL_RG8;
							break;
						case FX::Nodes::Variable::Properties::RG16:
							name = Texture::PixelFormat::RG16;
							internalformat = internalformatsrgb = GL_RG16;
							break;
						case FX::Nodes::Variable::Properties::RG16F:
							name = Texture::PixelFormat::RG16F;
							internalformat = internalformatsrgb = GL_RG16F;
							break;
						case FX::Nodes::Variable::Properties::RG32F:
							name = Texture::PixelFormat::RG32F;
							internalformat = internalformatsrgb = GL_RG32F;
							break;
						case FX::Nodes::Variable::Properties::RGBA8:
							name = Texture::PixelFormat::RGBA8;
							internalformat = GL_RGBA8;
							internalformatsrgb = GL_SRGB8_ALPHA8;
							break;
						case FX::Nodes::Variable::Properties::RGBA16:
							name = Texture::PixelFormat::RGBA16;
							internalformat = internalformatsrgb = GL_RGBA16;
							break;
						case FX::Nodes::Variable::Properties::RGBA16F:
							name = Texture::PixelFormat::RGBA16F;
							internalformat = internalformatsrgb = GL_RGBA16F;
							break;
						case FX::Nodes::Variable::Properties::RGBA32F:
							name = Texture::PixelFormat::RGBA32F;
							internalformat = internalformatsrgb = GL_RGBA32F;
							break;
						case FX::Nodes::Variable::Properties::DXT1:
							name = Texture::PixelFormat::DXT1;
							internalformat = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
							internalformatsrgb = GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT;
							break;
						case FX::Nodes::Variable::Properties::DXT3:
							name = Texture::PixelFormat::DXT3;
							internalformat = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
							internalformatsrgb = GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT;
							break;
						case FX::Nodes::Variable::Properties::DXT5:
							name = Texture::PixelFormat::DXT5;
							internalformat = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
							internalformatsrgb = GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT;
							break;
						case FX::Nodes::Variable::Properties::LATC1:
							name = Texture::PixelFormat::LATC1;
							internalformat = internalformatsrgb = GL_COMPRESSED_LUMINANCE_LATC1_EXT;
							break;
						case FX::Nodes::Variable::Properties::LATC2:
							name = Texture::PixelFormat::LATC2;
							internalformat = internalformatsrgb = GL_COMPRESSED_LUMINANCE_ALPHA_LATC2_EXT;
							break;
						default:
							name = Texture::PixelFormat::Unknown;
							internalformat = internalformatsrgb = GL_NONE;
							break;
					}
				}
				static std::string FixName(const std::string &name, const std::string &ns)
				{
					std::string res = boost::replace_all_copy(ns, "::", "_NS");

					if (boost::starts_with(name, "gl_") ||
						name == "common" || name == "partition" || name == "input" || name == "ouput" || name == "active" || name == "filter" || name == "superp" ||
						name == "invariant" || name == "lowp" || name == "mediump" || name == "highp" || name == "precision" || name == "patch" || name == "subroutine" ||
						name == "abs" || name == "sign" || name == "all" || name == "any" || name == "sin" || name == "sinh" || name == "cos" || name == "cosh" || name == "tan" || name == "tanh" || name == "asin" || name == "acos" || name == "atan" || name == "exp" || name == "exp2" || name == "log" || name == "log2" || name == "sqrt" || name == "inversesqrt" || name == "ceil" || name == "floor" || name == "fract" || name == "trunc" || name == "round" || name == "radians" || name == "degrees" || name == "length" || name == "normalize" || name == "transpose" || name == "determinant" || name == "intBitsToFloat" || name == "uintBitsToFloat" || name == "floatBitsToInt" || name == "floatBitsToUint" || name == "matrixCompMult" || name == "not" || name == "lessThan" || name == "greaterThan" || name == "lessThanEqual" || name == "greaterThanEqual" || name == "equal" || name == "notEqual" || name == "dot" || name == "cross" || name == "distance" || name == "pow" || name == "modf" || name == "frexp" || name == "ldexp" || name == "min" || name == "max" || name == "step" || name == "reflect" || name == "texture" || name == "textureOffset" || name == "fma" || name == "mix" || name == "clamp" || name == "smoothstep" || name == "refract" || name == "faceforward" || name == "textureLod" || name == "textureLodOffset" || name == "texelFetch" || name == "main")
					{
						res += '_';
					}

					res += boost::replace_all_copy(name, "__", "_US");

					return res;
				}
				static std::string FixNameWithSemantic(const std::string &name, const std::string &semantic, GLuint shadertype)
				{
					if (semantic == "SV_VERTEXID" || semantic == "VERTEXID")
					{
						return "gl_VertexID";
					}
					else if (semantic == "SV_INSTANCEID")
					{
						return "gl_InstanceID";
					}
					else if ((semantic == "SV_POSITION" || semantic == "POSITION") && shadertype == GL_VERTEX_SHADER)
					{
						return "gl_Position";
					}
					else if ((semantic == "SV_POSITION" || semantic == "VPOS") && shadertype == GL_FRAGMENT_SHADER)
					{
						return "gl_FragCoord";
					}
					else if ((semantic == "SV_DEPTH" || semantic == "DEPTH") && shadertype == GL_FRAGMENT_SHADER)
					{
						return "gl_FragDepth";
					}

					return FixName(name, "");
				}

			private:
				void Error(const FX::Location &location, const char *message, ...)
				{
					char formatted[512];

					va_list args;
					va_start(args, message);
					vsprintf_s(formatted, message, args);
					va_end(args);

					_errors += location.Source + "(" + std::to_string(location.Line) + ", " + std::to_string(location.Column) + "): error: " + formatted + '\n';
					_isFatal = true;
				}
				void Warning(const FX::Location &location, const char *message, ...)
				{
					char formatted[512];

					va_list args;
					va_start(args, message);
					vsprintf_s(formatted, message, args);
					va_end(args);

					_errors += location.Source + "(" + std::to_string(location.Line) + ", " + std::to_string(location.Column) + "): warning: " + formatted + '\n';
				}

				void VisitType(std::string &output, const FX::Nodes::Type &type)
				{
					if (type.HasQualifier(FX::Nodes::Type::Qualifier::Linear))
						output += "smooth ";
					if (type.HasQualifier(FX::Nodes::Type::Qualifier::NoPerspective))
						output += "noperspective ";
					if (type.HasQualifier(FX::Nodes::Type::Qualifier::Centroid))
						output += "centroid ";
					if (type.HasQualifier(FX::Nodes::Type::Qualifier::NoInterpolation))
						output += "flat ";
					if (type.HasQualifier(FX::Nodes::Type::Qualifier::InOut))
						output += "inout ";
					else if (type.HasQualifier(FX::Nodes::Type::Qualifier::In))
						output += "in ";
					else if (type.HasQualifier(FX::Nodes::Type::Qualifier::Out))
						output += "out ";
					else if (type.HasQualifier(FX::Nodes::Type::Qualifier::Uniform))
						output += "uniform ";
					if (type.HasQualifier(FX::Nodes::Type::Qualifier::Const))
						output += "const ";

					VisitTypeClass(output, type);
				}
				void VisitTypeClass(std::string &output, const FX::Nodes::Type &type)
				{
					switch (type.BaseClass)
					{
						case FX::Nodes::Type::Class::Void:
							output += "void";
							break;
						case FX::Nodes::Type::Class::Bool:
							if (type.IsMatrix())
								output += "mat" + std::to_string(type.Rows) + "x" + std::to_string(type.Cols);
							else if (type.IsVector())
								output += "bvec" + std::to_string(type.Rows);
							else
								output += "bool";
							break;
						case FX::Nodes::Type::Class::Int:
							if (type.IsMatrix())
								output += "mat" + std::to_string(type.Rows) + "x" + std::to_string(type.Cols);
							else if (type.IsVector())
								output += "ivec" + std::to_string(type.Rows);
							else
								output += "int";
							break;
						case FX::Nodes::Type::Class::Uint:
							if (type.IsMatrix())
								output += "mat" + std::to_string(type.Rows) + "x" + std::to_string(type.Cols);
							else if (type.IsVector())
								output += "uvec" + std::to_string(type.Rows);
							else
								output += "uint";
							break;
						case FX::Nodes::Type::Class::Float:
							if (type.IsMatrix())
								output += "mat" + std::to_string(type.Rows) + "x" + std::to_string(type.Cols);
							else if (type.IsVector())
								output += "vec" + std::to_string(type.Rows);
							else
								output += "float";
							break;
						case FX::Nodes::Type::Class::Sampler2D:
							output += "sampler2D";
							break;
						case FX::Nodes::Type::Class::Struct:
							output += FixName(type.Definition->Name, type.Definition->Namespace);
							break;
					}
				}
				std::pair<std::string, std::string> PrintCast(const FX::Nodes::Type &from, const FX::Nodes::Type &to)
				{
					std::pair<std::string, std::string> code;

					if (from.BaseClass != to.BaseClass && !(from.IsMatrix() && to.IsMatrix()))
					{
						const FX::Nodes::Type type = { to.BaseClass, 0, from.Rows, from.Cols, 0, to.Definition };

						VisitType(code.first, type);
						code.first += '(';
						code.second += ')';
					}

					if (from.Rows > 0 && from.Rows < to.Rows)
					{
						const char subscript[4] = { 'x', 'y', 'z', 'w' };

						code.second += '.';

						for (unsigned int i = 0; i < from.Rows; ++i)
						{
							code.second += subscript[i];
						}
						for (unsigned int i = from.Rows; i < to.Rows; ++i)
						{
							code.second += subscript[from.Rows - 1];
						}
					}
					else if (from.Rows > to.Rows)
					{
						const char subscript[4] = { 'x', 'y', 'z', 'w' };

						code.second += '.';

						for (unsigned int i = 0; i < to.Rows; ++i)
						{
							code.second += subscript[i];
						}
					}

					return code;
				}

				void Visit(std::string &output, const FX::Nodes::Statement *node)
				{
					if (node == nullptr)
					{
						return;
					}

					switch (node->NodeId)
					{
						case FX::Node::Id::Compound:
							Visit(output, static_cast<const FX::Nodes::Compound *>(node));
							break;
						case FX::Node::Id::DeclaratorList:
							Visit(output, static_cast<const FX::Nodes::DeclaratorList *>(node));
							break;
						case FX::Node::Id::ExpressionStatement:
							Visit(output, static_cast<const FX::Nodes::ExpressionStatement *>(node));
							break;
						case FX::Node::Id::If:
							Visit(output, static_cast<const FX::Nodes::If *>(node));
							break;
						case FX::Node::Id::Switch:
							Visit(output, static_cast<const FX::Nodes::Switch *>(node));
							break;
						case FX::Node::Id::For:
							Visit(output, static_cast<const FX::Nodes::For *>(node));
							break;
						case FX::Node::Id::While:
							Visit(output, static_cast<const FX::Nodes::While *>(node));
							break;
						case FX::Node::Id::Return:
							Visit(output, static_cast<const FX::Nodes::Return *>(node));
							break;
						case FX::Node::Id::Jump:
							Visit(output, static_cast<const FX::Nodes::Jump *>(node));
							break;
						default:
							assert(false);
							break;
					}
				}
				void Visit(std::string &output, const FX::Nodes::Expression *node)
				{
					switch (node->NodeId)
					{
						case FX::Node::Id::LValue:
							Visit(output, static_cast<const FX::Nodes::LValue *>(node));
							break;
						case FX::Node::Id::Literal:
							Visit(output, static_cast<const FX::Nodes::Literal *>(node));
							break;
						case FX::Node::Id::Sequence:
							Visit(output, static_cast<const FX::Nodes::Sequence *>(node));
							break;
						case FX::Node::Id::Unary:
							Visit(output, static_cast<const FX::Nodes::Unary *>(node));
							break;
						case FX::Node::Id::Binary:
							Visit(output, static_cast<const FX::Nodes::Binary *>(node));
							break;
						case FX::Node::Id::Intrinsic:
							Visit(output, static_cast<const FX::Nodes::Intrinsic *>(node));
							break;
						case FX::Node::Id::Conditional:
							Visit(output, static_cast<const FX::Nodes::Conditional *>(node));
							break;
						case FX::Node::Id::Swizzle:
							Visit(output, static_cast<const FX::Nodes::Swizzle *>(node));
							break;
						case FX::Node::Id::FieldSelection:
							Visit(output, static_cast<const FX::Nodes::FieldSelection *>(node));
							break;
						case FX::Node::Id::Assignment:
							Visit(output, static_cast<const FX::Nodes::Assignment *>(node));
							break;
						case FX::Node::Id::Call:
							Visit(output, static_cast<const FX::Nodes::Call *>(node));
							break;
						case FX::Node::Id::Constructor:
							Visit(output, static_cast<const FX::Nodes::Constructor *>(node));
							break;
						case FX::Node::Id::InitializerList:
							Visit(output, static_cast<const FX::Nodes::InitializerList *>(node));
							break;
						default:
							assert(false);
							break;
					}
				}

				void Visit(std::string &output, const FX::Nodes::Compound *node)
				{
					output += "{\n";

					for (auto statement : node->Statements)
					{
						Visit(output, statement);
					}

					output += "}\n";
				}
				void Visit(std::string &output, const FX::Nodes::DeclaratorList *node, bool singlestatement = false)
				{
					bool includetype = true;

					for (auto declarator : node->Declarators)
					{
						Visit(output, declarator, includetype);

						if (singlestatement)
						{
							output += ", ";

							includetype = false;
						}
						else
						{
							output += ";\n";
						}
					}

					if (singlestatement)
					{
						output.erase(output.end() - 2, output.end());

						output += ";\n";
					}
				}
				void Visit(std::string &output, const FX::Nodes::ExpressionStatement *node)
				{
					Visit(output, node->Expression);

					output += ";\n";
				}
				void Visit(std::string &output, const FX::Nodes::If *node)
				{
					const FX::Nodes::Type typeto = { FX::Nodes::Type::Class::Bool, 0, 1, 1 };
					const auto cast = PrintCast(node->Condition->Type, typeto);

					output += "if (" + cast.first;

					Visit(output, node->Condition);

					output += cast.second + ")\n";

					if (node->StatementOnTrue != nullptr)
					{
						Visit(output, node->StatementOnTrue);
					}
					else
					{
						output += "\t;";
					}

					if (node->StatementOnFalse != nullptr)
					{
						output += "else\n";

						Visit(output, node->StatementOnFalse);
					}
				}
				void Visit(std::string &output, const FX::Nodes::Switch *node)
				{
					output += "switch (";

					Visit(output, node->Test);

					output += ")\n{\n";

					for (auto currcase : node->Cases)
					{
						Visit(output, currcase);
					}

					output += "}\n";
				}
				void Visit(std::string &output, const FX::Nodes::Case *node)
				{
					for (auto label : node->Labels)
					{
						if (label == nullptr)
						{
							output += "default";
						}
						else
						{
							output += "case ";

							Visit(output, label);
						}

						output += ":\n";
					}

					Visit(output, node->Statements);
				}
				void Visit(std::string &output, const FX::Nodes::For *node)
				{
					output += "for (";

					if (node->Initialization != nullptr)
					{
						if (node->Initialization->NodeId == FX::Node::Id::DeclaratorList)
						{
							Visit(output, static_cast<FX::Nodes::DeclaratorList *>(node->Initialization), true);

							output.erase(output.end() - 2, output.end());
						}
						else
						{
							Visit(output, static_cast<FX::Nodes::ExpressionStatement *>(node->Initialization)->Expression);
						}
					}

					output += "; ";

					if (node->Condition != nullptr)
					{
						Visit(output, node->Condition);
					}

					output += "; ";

					if (node->Increment != nullptr)
					{
						Visit(output, node->Increment);
					}

					output += ")\n";

					if (node->Statements != nullptr)
					{
						Visit(output, node->Statements);
					}
					else
					{
						output += "\t;";
					}
				}
				void Visit(std::string &output, const FX::Nodes::While *node)
				{
					if (node->DoWhile)
					{
						output += "do\n{\n";

						if (node->Statements != nullptr)
						{
							Visit(output, node->Statements);
						}

						output += "}\nwhile (";

						Visit(output, node->Condition);

						output += ");\n";
					}
					else
					{
						output += "while (";

						Visit(output, node->Condition);

						output += ")\n";

						if (node->Statements != nullptr)
						{
							Visit(output, node->Statements);
						}
						else
						{
							output += "\t;";
						}
					}
				}
				void Visit(std::string &output, const FX::Nodes::Return *node)
				{
					if (node->Discard)
					{
						output += "discard";
					}
					else
					{
						output += "return";

						if (node->Value != nullptr)
						{
							assert(_currentFunction != nullptr);

							const auto cast = PrintCast(node->Value->Type, _currentFunction->ReturnType);

							output += ' ' + cast.first;
							
							Visit(output, node->Value);
							
							output += cast.second;
						}
					}

					output += ";\n";
				}
				void Visit(std::string &output, const FX::Nodes::Jump *node)
				{
					switch (node->Mode)
					{
						case FX::Nodes::Jump::Break:
							output += "break";
							break;
						case FX::Nodes::Jump::Continue:
							output += "continue";
							break;
					}

					output += ";\n";
				}

				void Visit(std::string &output, const FX::Nodes::LValue *node)
				{
					output += FixName(node->Reference->Name, node->Reference->Namespace);
				}
				void Visit(std::string &output, const FX::Nodes::Literal *node)
				{
					if (!node->Type.IsScalar())
					{
						VisitTypeClass(output, node->Type);

						output += '(';
					}

					for (unsigned int i = 0; i < node->Type.Rows * node->Type.Cols; ++i)
					{
						switch (node->Type.BaseClass)
						{
							case FX::Nodes::Type::Class::Bool:
								output += node->Value.Int[i] ? "true" : "false";
								break;
							case FX::Nodes::Type::Class::Int:
								output += std::to_string(node->Value.Int[i]);
								break;
							case FX::Nodes::Type::Class::Uint:
								output += std::to_string(node->Value.Uint[i]) + "u";
								break;
							case FX::Nodes::Type::Class::Float:
								output += std::to_string(node->Value.Float[i]);
								break;
						}

						output += ", ";
					}

					output.erase(output.end() - 2, output.end());

					if (!node->Type.IsScalar())
					{
						output += ')';
					}
				}
				void Visit(std::string &output, const FX::Nodes::Sequence *node)
				{
					output += '(';

					for (auto expression : node->Expressions)
					{
						Visit(output, expression);

						output += ", ";
					}

					output.erase(output.end() - 2, output.end());

					output += ')';
				}
				void Visit(std::string &output, const FX::Nodes::Unary *node)
				{
					std::string part1, part2;

					switch (node->Operator)
					{
						case FX::Nodes::Unary::Op::Negate:
							part1 = '-';
							break;
						case FX::Nodes::Unary::Op::BitwiseNot:
							part1 = "~";
							break;
						case FX::Nodes::Unary::Op::LogicalNot:
							if (node->Type.IsVector())
							{
								const auto cast = PrintCast(node->Operand->Type, node->Type);

								part1 = "not(" + cast.first;
								part2 = cast.second + ')';
							}
							else
							{
								part1 = "!bool(";
								part2 = ')';
							}
							break;
						case FX::Nodes::Unary::Op::Increase:
							part1 = "++";
							break;
						case FX::Nodes::Unary::Op::Decrease:
							part1 = "--";
							break;
						case FX::Nodes::Unary::Op::PostIncrease:
							part2 = "++";
							break;
						case FX::Nodes::Unary::Op::PostDecrease:
							part2 = "--";
							break;
						case FX::Nodes::Unary::Op::Cast:
							VisitTypeClass(part1, node->Type);
							part1 += '(';
							part2 = ')';
							break;
					}

					output += part1;
					Visit(output, node->Operand);
					output += part2;
				}
				void Visit(std::string &output, const FX::Nodes::Binary *node)
				{
					const auto type1 = node->Operands[0]->Type;
					const auto type2 = node->Operands[1]->Type;				
					auto type12 = type2.IsFloatingPoint() ? type2 : type1;
					type12.Rows = std::max(type1.Rows, type2.Rows);
					type12.Cols = std::max(type1.Cols, type2.Cols);

					const auto cast1 = PrintCast(type1, node->Type), cast2 = PrintCast(type2, node->Type);
					const auto cast121 = PrintCast(type1, type12), cast122 = PrintCast(type2, type12);

					std::string part1, part2, part3;

					switch (node->Operator)
					{
						case FX::Nodes::Binary::Op::Add:
							part1 = '(' + cast1.first;
							part2 = cast1.second + " + " + cast2.first;
							part3 = cast2.second + ')';
							break;
						case FX::Nodes::Binary::Op::Subtract:
							part1 = '(' + cast1.first;
							part2 = cast1.second + " - " + cast2.first;
							part3 = cast2.second + ')';
							break;
						case FX::Nodes::Binary::Op::Multiply:
							if (node->Type.IsMatrix())
							{
								part1 = "matrixCompMult(" + cast1.first;
								part2 = cast1.second + ", " + cast2.first;
								part3 = cast2.second + ')';
							}
							else
							{
								part1 = '(' + cast1.first;
								part2 = cast1.second + " * " + cast2.first;
								part3 = cast2.second + ')';
							}
							break;
						case FX::Nodes::Binary::Op::Divide:
							part1 = '(' + cast1.first;
							part2 = cast1.second + " / " + cast2.first;
							part3 = cast2.second + ')';
							break;
						case FX::Nodes::Binary::Op::Modulo:
							if (node->Type.IsFloatingPoint())
							{
								part1 = "_fmod(" + cast1.first;
								part2 = cast1.second + ", " + cast2.first;
								part3 = cast2.second + ')';
							}
							else
							{
								part1 = '(' + cast1.first;
								part2 = cast1.second + " % " + cast2.first;
								part3 = cast2.second + ')';
							}
							break;
						case FX::Nodes::Binary::Op::Less:
							if (node->Type.IsVector())
							{
								part1 = "lessThan(" + cast121.first;
								part2 = cast121.second + ", " + cast122.first;
								part3 = cast122.second + ')';
							}
							else
							{
								part1 = '(' + cast121.first;
								part2 = cast121.second + " < " + cast122.first;
								part3 = cast122.second + ')';
							}
							break;
						case FX::Nodes::Binary::Op::Greater:
							if (node->Type.IsVector())
							{
								part1 = "greaterThan(" + cast121.first;
								part2 = cast121.second + ", " + cast122.first;
								part3 = cast122.second + ')';
							}
							else
							{
								part1 = '(' + cast121.first;
								part2 = cast121.second + " > " + cast122.first;
								part3 = cast122.second + ')';
							}
							break;
						case FX::Nodes::Binary::Op::LessOrEqual:
							if (node->Type.IsVector())
							{
								part1 = "lessThanEqual(" + cast121.first;
								part2 = cast121.second + ", " + cast122.first;
								part3 = cast122.second + ')';
							}
							else
							{
								part1 = '(' + cast121.first;
								part2 = cast121.second + " <= " + cast122.first;
								part3 = cast122.second + ')';
							}
							break;
						case FX::Nodes::Binary::Op::GreaterOrEqual:
							if (node->Type.IsVector())
							{
								part1 = "greaterThanEqual(" + cast121.first;
								part2 = cast121.second + ", " + cast122.first;
								part3 = cast122.second + ')';
							}
							else
							{
								part1 = '(' + cast121.first;
								part2 = cast121.second + " >= " + cast122.first;
								part3 = cast122.second + ')';
							}
							break;
						case FX::Nodes::Binary::Op::Equal:
							if (node->Type.IsVector())
							{
								part1 = "equal(" + cast121.first;
								part2 = cast121.second + ", " + cast122.first;
								part3 = cast122.second + ")";
							}
							else
							{
								part1 = '(' + cast121.first;
								part2 = cast121.second + " == " + cast122.first;
								part3 = cast122.second + ')';
							}
							break;
						case FX::Nodes::Binary::Op::NotEqual:
							if (node->Type.IsVector())
							{
								part1 = "notEqual(" + cast121.first;
								part2 = cast121.second + ", " + cast122.first;
								part3 = cast122.second + ")";
							}
							else
							{
								part1 = '(' + cast121.first;
								part2 = cast121.second + " != " + cast122.first;
								part3 = cast122.second + ')';
							}
							break;
						case FX::Nodes::Binary::Op::LeftShift:
							part1 = '(';
							part2 = " << ";
							part3 = ')';
							break;
						case FX::Nodes::Binary::Op::RightShift:
							part1 = '(';
							part2 = " >> ";
							part3 = ')';
							break;
						case FX::Nodes::Binary::Op::BitwiseAnd:
							part1 = '(' + cast1.first;
							part2 = cast1.second + " & " + cast2.first;
							part3 = cast2.second + ')';
							break;
						case FX::Nodes::Binary::Op::BitwiseOr:
							part1 = '(' + cast1.first;
							part2 = cast1.second + " | " + cast2.first;
							part3 = cast2.second + ')';
							break;
						case FX::Nodes::Binary::Op::BitwiseXor:
							part1 = '(' + cast1.first;
							part2 = cast1.second + " ^ " + cast2.first;
							part3 = cast2.second + ')';
							break;
						case FX::Nodes::Binary::Op::LogicalAnd:
							part1 = '(' + cast121.first;
							part2 = cast121.second + " && " + cast122.first;
							part3 = cast122.second + ')';
							break;
						case FX::Nodes::Binary::Op::LogicalOr:
							part1 = '(' + cast121.first;
							part2 = cast121.second + " || " + cast122.first;
							part3 = cast122.second + ')';
							break;
						case FX::Nodes::Binary::Op::ElementExtract:
							if (type2.BaseClass != FX::Nodes::Type::Class::Uint)
							{
								part2 = "[uint(";
								part3 = ")]";
							}
							else
							{
								part2 = '[';
								part3 = ']';
							}
							break;
					}

					output += part1;
					Visit(output, node->Operands[0]);
					output += part2;
					Visit(output, node->Operands[1]);
					output += part3;
				}
				void Visit(std::string &output, const FX::Nodes::Intrinsic *node)
				{
					FX::Nodes::Type type1 = { FX::Nodes::Type::Class::Void }, type2, type3, type4, type12;
					std::pair<std::string, std::string> cast1, cast2, cast3, cast4, cast121, cast122;

					if (node->Arguments[0] != nullptr)
					{
						cast1 = PrintCast(type1 = node->Arguments[0]->Type, node->Type);
					}
					if (node->Arguments[1] != nullptr)
					{
						cast2 = PrintCast(type2 = node->Arguments[1]->Type, node->Type);

						type12 = type2.IsFloatingPoint() ? type2 : type1;
						type12.Rows = std::max(type1.Rows, type2.Rows);
						type12.Cols = std::max(type1.Cols, type2.Cols);

						cast121 = PrintCast(type1, type12);
						cast122 = PrintCast(type2, type12);
					}
					if (node->Arguments[2] != nullptr)
					{
						cast3 = PrintCast(type3 = node->Arguments[2]->Type, node->Type);
					}
					if (node->Arguments[3] != nullptr)
					{
						cast4 = PrintCast(type4 = node->Arguments[2]->Type, node->Type);
					}

					std::string part1, part2, part3, part4, part5;

					switch (node->Operator)
					{
						case FX::Nodes::Intrinsic::Op::Abs:
							part1 = "abs(" + cast1.first;
							part2 = cast1.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::Acos:
							part1 = "acos(" + cast1.first;
							part2 = cast1.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::All:
							if (type1.IsVector())
							{
								part1 = "all(bvec" + std::to_string(type1.Rows) + '(';
								part2 = "))";
							}
							else
							{
								part1 = "bool(";
								part2 = ')';
							}
							break;
						case FX::Nodes::Intrinsic::Op::Any:
							if (type1.IsVector())
							{
								part1 = "any(bvec" + std::to_string(type1.Rows) + '(';
								part2 = "))";
							}
							else
							{
								part1 = "bool(";
								part2 = ')';
							}
							break;
						case FX::Nodes::Intrinsic::Op::BitCastInt2Float:
							part1 = "intBitsToFloat(";

							if (type1.BaseClass != FX::Nodes::Type::Class::Int)
							{
								type1.BaseClass = FX::Nodes::Type::Class::Int;

								VisitTypeClass(part1, type1);
								part1 += '(';
								part2 = ')';
							}

							part2 += ')';
							break;
						case FX::Nodes::Intrinsic::Op::BitCastUint2Float:
							part1 = "uintBitsToFloat(";

							if (type1.BaseClass != FX::Nodes::Type::Class::Uint)
							{
								type1.BaseClass = FX::Nodes::Type::Class::Uint;
								
								VisitTypeClass(part1, type1);
								part1 += '(';
								part2 = ')';
							}

							part2 += ')';
							break;
						case FX::Nodes::Intrinsic::Op::Asin:
							part1 = "asin(" + cast1.first;
							part2 = cast1.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::BitCastFloat2Int:
							part1 = "floatBitsToInt(";

							if (type1.BaseClass != FX::Nodes::Type::Class::Float)
							{
								type1.BaseClass = FX::Nodes::Type::Class::Float;
								
								VisitTypeClass(part1, type1);
								part1 += '(';
								part2 = ')';
							}

							part2 += ')';
							break;
						case FX::Nodes::Intrinsic::Op::BitCastFloat2Uint:
							part1 = "floatBitsToUint(";

							if (type1.BaseClass != FX::Nodes::Type::Class::Float)
							{
								type1.BaseClass = FX::Nodes::Type::Class::Float;
								
								VisitTypeClass(part1, type1);
								part1 += '(';
								part2 = ')';
							}

							part2 += ')';
							break;
						case FX::Nodes::Intrinsic::Op::Atan:
							part1 = "atan(" + cast1.first;
							part2 = cast1.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::Atan2:
							part1 = "atan(" + cast1.first;
							part2 = cast1.second + ", " + cast2.first;
							part3 = cast2.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::Ceil:
							part1 = "ceil(" + cast1.first;
							part2 = cast1.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::Clamp:
							part1 = "clamp(" + cast1.first;
							part2 = cast1.second + ", " + cast2.first;
							part3 = cast2.second + ", " + cast3.first;
							part4 = cast3.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::Cos:
							part1 = "cos(" + cast1.first;
							part2 = cast1.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::Cosh:
							part1 = "cosh(" + cast1.first;
							part2 = cast1.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::Cross:
							part1 = "cross(" + cast1.first;
							part2 = cast1.second + ", " + cast2.first;
							part3 = cast2.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::PartialDerivativeX:
							part1 = "dFdx(" + cast1.first;
							part2 = cast1.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::PartialDerivativeY:
							part1 = "dFdy(" + cast1.first;
							part2 = cast1.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::Degrees:
							part1 = "degrees(" + cast1.first;
							part2 = cast1.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::Determinant:
							part1 = "determinant(";

							if (!type1.IsFloatingPoint())
							{
								type1.BaseClass = FX::Nodes::Type::Class::Float;
								
								VisitTypeClass(part1, type1);
								part1 += '(';
								part2 = ')';
							}

							part2 += ')';
							break;
						case FX::Nodes::Intrinsic::Op::Distance:
							part1 = "distance(" + cast121.first;
							part2 = cast121.second + ", " + cast122.first;
							part3 = cast122.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::Dot:
							part1 = "dot(" + cast121.first;
							part2 = cast121.second + ", " + cast122.first;
							part3 = cast122.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::Exp:
							part1 = "exp(" + cast1.first;
							part2 = cast1.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::Exp2:
							part1 = "exp2(" + cast1.first;
							part2 = cast1.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::FaceForward:
							part1 = "faceforward(" + cast1.first;
							part2 = cast1.second + ", " + cast2.first;
							part3 = cast2.second + ", " + cast3.first;
							part4 = cast3.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::Floor:
							part1 = "floor(" + cast1.first;
							part2 = cast1.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::Frac:
							part1 = "fract(" + cast1.first;
							part2 = cast1.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::Frexp:
							part1 = "frexp(" + cast1.first;
							part2 = cast1.second + ", " + cast2.first;
							part3 = cast2.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::Fwidth:
							part1 = "fwidth(" + cast1.first;
							part2 = cast1.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::Ldexp:
							part1 = "ldexp(" + cast1.first;
							part2 = cast1.second + ", " + cast2.first;
							part3 = cast2.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::Length:
							part1 = "length(";

							if (!type1.IsFloatingPoint())
							{
								type1.BaseClass = FX::Nodes::Type::Class::Float;
								
								VisitTypeClass(part1, type1);
								part1 += '(';
								part2 = ')';
							}

							part2 += ')';
							break;
						case FX::Nodes::Intrinsic::Op::Lerp:
							part1 = "mix(" + cast1.first;
							part2 = cast1.second + ", " + cast2.first;
							part3 = cast2.second + ", " + cast3.first;
							part4 = cast3.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::Log:
							part1 = "log(" + cast1.first;
							part2 = cast1.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::Log10:
							part1 = "(log2(" + cast1.first;
							part2 = cast1.second + ") / ";
							VisitTypeClass(part2, node->Type);
							part2 += "(2.302585093))";
							break;
						case FX::Nodes::Intrinsic::Op::Log2:
							part1 = "log2(" + cast1.first;
							part2 = cast1.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::Mad:
							part1 = "(" + cast1.first;
							part2 = cast1.second + " * " + cast2.first;
							part3 = cast2.second + " + " + cast3.first;
							part4 = cast3.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::Max:
							part1 = "max(" + cast1.first;
							part2 = cast1.second + ", " + cast2.first;
							part3 = cast2.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::Min:
							part1 = "min(" + cast1.first;
							part2 = cast1.second + ", " + cast2.first;
							part3 = cast2.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::Modf:
							part1 = "modf(" + cast1.first;
							part2 = cast1.second + ", " + cast2.first;
							part3 = cast2.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::Mul:
							part1 = '(';
							part2 = " * ";
							part3 = ')';
							break;
						case FX::Nodes::Intrinsic::Op::Normalize:
							part1 = "normalize(";

							if (!type1.IsFloatingPoint())
							{
								type1.BaseClass = FX::Nodes::Type::Class::Float;
								
								VisitTypeClass(part1, type1);
								part1 += '(';
								part2 = ')';
							}

							part2 += ')';
							break;
						case FX::Nodes::Intrinsic::Op::Pow:
							part1 = "pow(" + cast1.first;
							part2 = cast1.second + ", " + cast2.first;
							part3 = cast2.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::Radians:
							part1 = "radians(" + cast1.first;
							part2 = cast1.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::Rcp:
							part1 = '(';
							VisitTypeClass(part1, node->Type);
							part1 += "(1.0) / ";
							part2 = ')';
							break;
						case FX::Nodes::Intrinsic::Op::Reflect:
							part1 = "reflect(" + cast1.first;
							part2 = cast1.second + ", " + cast2.first;
							part3 = cast2.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::Refract:
							part1 = "refract(" + cast1.first;
							part2 = cast1.second + ", " + cast2.first;
							part3 = cast2.second + ", float";
							part4 = "))";
							break;
						case FX::Nodes::Intrinsic::Op::Round:
							part1 = "round(" + cast1.first;
							part2 = cast1.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::Rsqrt:
							part1 = "inversesqrt(" + cast1.first;
							part2 = cast1.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::Saturate:
							part1 = "clamp(" + cast1.first;
							part2 = cast1.second + ", 0.0, 1.0)";
							break;
						case FX::Nodes::Intrinsic::Op::Sign:
							part1 = cast1.first + "sign(";
							part2 = ')' + cast1.second;
							break;
						case FX::Nodes::Intrinsic::Op::Sin:
							part1 = "sin(" + cast1.first;
							part2 = cast1.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::SinCos:
							part1 = "_sincos(";

							if (type1.BaseClass != FX::Nodes::Type::Class::Float)
							{
								type1.BaseClass = FX::Nodes::Type::Class::Float;
								
								VisitTypeClass(part1, type1);
								part1 += '(';
								part2 = ')';
							}

							part2 = ", ";
							part3 = ", ";
							part4 = ')';
							break;
						case FX::Nodes::Intrinsic::Op::Sinh:
							part1 = "sinh(" + cast1.first;
							part2 = cast1.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::SmoothStep:
							part1 = "smoothstep(" + cast1.first;
							part2 = cast1.second + ", " + cast2.first;
							part3 = cast2.second + ", " + cast3.first;
							part4 = cast3.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::Sqrt:
							part1 = "sqrt(" + cast1.first;
							part2 = cast1.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::Step:
							part1 = "step(" + cast1.first;
							part2 = cast1.second + ", " + cast2.first;
							part3 = cast2.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::Tan:
							part1 = "tan(" + cast1.first;
							part2 = cast1.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::Tanh:
							part1 = "tanh(" + cast1.first;
							part2 = cast1.second + ')';
							break;
						case FX::Nodes::Intrinsic::Op::Tex2D:
						{
							const FX::Nodes::Type type2to = { FX::Nodes::Type::Class::Float, 0, 2, 1 };
							cast2 = PrintCast(type2, type2to);

							part1 = "texture(";
							part2 = ", " + cast2.first;
							part3 = cast2.second + " * vec2(1.0, -1.0) + vec2(0.0, 1.0))";
							break;
						}
						case FX::Nodes::Intrinsic::Op::Tex2DFetch:
						{
							const FX::Nodes::Type type2to = { FX::Nodes::Type::Class::Int, 0, 2, 1 };
							cast2 = PrintCast(type2, type2to);

							part1 = "texelFetch(";
							part2 = ", " + cast2.first;
							part3 = cast2.second + " * ivec2(1, -1) + ivec2(0, 1))";
							break;
						}
						case FX::Nodes::Intrinsic::Op::Tex2DGather:
						{
							const FX::Nodes::Type type2to = { FX::Nodes::Type::Class::Float, 0, 2, 1 };
							cast2 = PrintCast(type2, type2to);

							part1 = "textureGather(";
							part2 = ", " + cast2.first;
							part3 = cast2.second + " * vec2(1.0, -1.0) + vec2(0.0, 1.0), int(";
							part4 = "))";
							break;
						}
						case FX::Nodes::Intrinsic::Op::Tex2DGatherOffset:
						{
							const FX::Nodes::Type type2to = { FX::Nodes::Type::Class::Float, 0, 2, 1 };
							const FX::Nodes::Type type3to = { FX::Nodes::Type::Class::Int, 0, 2, 1 };
							cast2 = PrintCast(type2, type2to);
							cast3 = PrintCast(type3, type3to);

							part1 = "textureGatherOffset(";
							part2 = ", " + cast2.first;
							part3 = cast2.second + " * vec2(1.0, -1.0) + vec2(0.0, 1.0), " + cast3.first;
							part4 = cast3.second + " * ivec2(1, -1), int(";
							part5 = "))";
							break;
						}
						case FX::Nodes::Intrinsic::Op::Tex2DGrad:
						{
							const FX::Nodes::Type type2to = { FX::Nodes::Type::Class::Float, 0, 2, 1 };
							cast2 = PrintCast(type2, type2to);
							cast3 = PrintCast(type3, type2to);
							cast4 = PrintCast(type4, type2to);

							part1 = "textureGrad(";
							part2 = ", " + cast2.first;
							part3 = cast2.second + " * vec2(1.0, -1.0) + vec2(0.0, 1.0), " + cast3.first;
							part4 = cast3.second + ", " + cast4.first;
							part5 = cast4.second + ")";
							break;
						}
						case FX::Nodes::Intrinsic::Op::Tex2DLevel:
						{
							const FX::Nodes::Type type2to = { FX::Nodes::Type::Class::Float, 0, 4, 1 };
							cast2 = PrintCast(type2, type2to);

							part1 = "_textureLod(";
							part2 = ", " + cast2.first;
							part3 = cast2.second + " * vec4(1.0, -1.0, 1.0, 1.0) + vec4(0.0, 1.0, 0.0, 0.0))";
							break;
						}
						case FX::Nodes::Intrinsic::Op::Tex2DLevelOffset:
						{	
							const FX::Nodes::Type type2to = { FX::Nodes::Type::Class::Float, 0, 4, 1 };
							const FX::Nodes::Type type3to = { FX::Nodes::Type::Class::Int, 0, 2, 1 };
							cast2 = PrintCast(type2, type2to);
							cast3 = PrintCast(type3, type3to);

							part1 = "_textureLodOffset(";
							part2 = ", " + cast2.first;
							part3 = cast2.second + " * vec4(1.0, -1.0, 1.0, 1.0) + vec4(0.0, 1.0, 0.0, 0.0), " + cast3.first;
							part4 = cast3.second + " * ivec2(1, -1))";
							break;
						}
						case FX::Nodes::Intrinsic::Op::Tex2DOffset:
						{
							const FX::Nodes::Type type2to = { FX::Nodes::Type::Class::Float, 0, 2, 1 };
							const FX::Nodes::Type type3to = { FX::Nodes::Type::Class::Int, 0, 2, 1 };
							cast2 = PrintCast(type2, type2to);
							cast3 = PrintCast(type3, type3to);

							part1 = "textureOffset(";
							part2 = ", " + cast2.first;
							part3 = cast2.second + " * vec2(1.0, -1.0) + vec2(0.0, 1.0), " + cast3.first;
							part4 = cast3.second + " * ivec2(1, -1))";
							break;
						}
						case FX::Nodes::Intrinsic::Op::Tex2DProj:
						{
							const FX::Nodes::Type type2to = { FX::Nodes::Type::Class::Float, 0, 4, 1 };
							cast2 = PrintCast(type2, type2to);

							part1 = "textureProj(";
							part2 = ", " + cast2.first;
							part3 = cast2.second + " * vec4(1.0, -1.0, 1.0, 1.0) + vec4(0.0, 1.0, 0.0, 0.0))";
							break;
						}
						case FX::Nodes::Intrinsic::Op::Tex2DSize:
							part1 = "textureSize(";
							part2 = ", int(";
							part3 = "))";
							break;
						case FX::Nodes::Intrinsic::Op::Transpose:
							part1 = "transpose(";

							if (!type1.IsFloatingPoint())
							{
								type1.BaseClass = FX::Nodes::Type::Class::Float;
								
								VisitTypeClass(part1, type1);
								part1 += '(';
								part2 = ')';
							}

							part2 += ')';
							break;
						case FX::Nodes::Intrinsic::Op::Trunc:
							part1 = "trunc(" + cast1.first;
							part2 = cast1.second + ')';
							break;
					}

					output += part1;

					if (node->Arguments[0] != nullptr)
					{
						Visit(output, node->Arguments[0]);
					}

					output += part2;

					if (node->Arguments[1] != nullptr)
					{
						Visit(output, node->Arguments[1]);
					}

					output += part3;

					if (node->Arguments[2] != nullptr)
					{
						Visit(output, node->Arguments[2]);
					}

					output += part4;

					if (node->Arguments[3] != nullptr)
					{
						Visit(output, node->Arguments[3]);
					}

					output += part5;
				}
				void Visit(std::string &output, const FX::Nodes::Conditional *node)
				{
					output += '(';

					if (node->Condition->Type.IsVector())
					{
						output += "all(bvec" + std::to_string(node->Condition->Type.Rows) + '(';
						
						Visit(output, node->Condition);
						
						output += "))";
					}
					else
					{
						output += "bool(";
						
						Visit(output, node->Condition);
						
						output += ')';
					}

					const std::pair<std::string, std::string> cast1 = PrintCast(node->ExpressionOnTrue->Type, node->Type);
					const std::pair<std::string, std::string> cast2 = PrintCast(node->ExpressionOnFalse->Type, node->Type);

					output += " ? " + cast1.first;
					Visit(output, node->ExpressionOnTrue);
					output += cast1.second + " : " + cast2.first;
					Visit(output, node->ExpressionOnFalse);
					output += cast2.second + ')';
				}
				void Visit(std::string &output, const FX::Nodes::Swizzle *node)
				{
					Visit(output, node->Operand);

					if (node->Operand->Type.IsMatrix())
					{
						if (node->Mask[1] >= 0)
						{
							Error(node->Location, "multiple component matrix swizzeling is not supported in OpenGL");
							return;
						}

						const unsigned int row = node->Mask[0] % 4;
						const unsigned int col = (node->Mask[0] - row) / 4;

						output += '[' + std::to_string(row) + "][" + std::to_string(col) + ']';
					}
					else
					{
						const char swizzle[4] = { 'x', 'y', 'z', 'w' };

						output += '.';

						for (int i = 0; i < 4 && node->Mask[i] >= 0; ++i)
						{
							output += swizzle[node->Mask[i]];
						}
					}
				}
				void Visit(std::string &output, const FX::Nodes::FieldSelection *node)
				{
					output += '(';

					Visit(output, node->Operand);

					output += '.';
					output += FixName(node->Field->Name, node->Field->Namespace);
					output += ')';
				}
				void Visit(std::string &output, const FX::Nodes::Assignment *node)
				{
					output += '(';
					
					Visit(output, node->Left);
					
					output += ' ';

					switch (node->Operator)
					{
						case FX::Nodes::Assignment::Op::None:
							output += '=';
							break;
						case FX::Nodes::Assignment::Op::Add:
							output += "+=";
							break;
						case FX::Nodes::Assignment::Op::Subtract:
							output += "-=";
							break;
						case FX::Nodes::Assignment::Op::Multiply:
							output += "*=";
							break;
						case FX::Nodes::Assignment::Op::Divide:
							output += "/=";
							break;
						case FX::Nodes::Assignment::Op::Modulo:
							output += "%=";
							break;
						case FX::Nodes::Assignment::Op::LeftShift:
							output += "<<=";
							break;
						case FX::Nodes::Assignment::Op::RightShift:
							output += ">>=";
							break;
						case FX::Nodes::Assignment::Op::BitwiseAnd:
							output += "&=";
							break;
						case FX::Nodes::Assignment::Op::BitwiseOr:
							output += "|=";
							break;
						case FX::Nodes::Assignment::Op::BitwiseXor:
							output += "^=";
							break;
					}

					const std::pair<std::string, std::string> cast = PrintCast(node->Right->Type, node->Left->Type);

					output += ' ' + cast.first;
					
					Visit(output, node->Right);
					
					output += cast.second + ')';
				}
				void Visit(std::string &output, const FX::Nodes::Call *node)
				{
					output += FixName(node->Callee->Name, node->Callee->Namespace) + '(';

					if (!node->Arguments.empty())
					{
						for (size_t i = 0, count = node->Arguments.size(); i < count; ++i)
						{
							const auto argument = node->Arguments[i];
							const auto parameter = node->Callee->Parameters[i];

							const std::pair<std::string , std::string> cast = PrintCast(argument->Type, parameter->Type);

							output += cast.first;

							Visit(output, argument);

							output += cast.second + ", ";
						}

						output.erase(output.end() - 2, output.end());
					}

					output += ')';

					auto &info = _functions.at(_currentFunction);
					auto &infoCallee = _functions.at(node->Callee);
					
					for (auto dependency : infoCallee.FunctionDependencies)
					{
						if (std::find(info.FunctionDependencies.begin(), info.FunctionDependencies.end(), dependency) == info.FunctionDependencies.end())
						{
							info.FunctionDependencies.push_back(dependency);
						}
					}

					if (std::find(info.FunctionDependencies.begin(), info.FunctionDependencies.end(), node->Callee) == info.FunctionDependencies.end())
					{
						info.FunctionDependencies.push_back(node->Callee);
					}
				}
				void Visit(std::string &output, const FX::Nodes::Constructor *node)
				{
					if (node->Type.IsMatrix())
					{
						output += "transpose(";
					}

					VisitTypeClass(output, node->Type);
					output += '(';

					if (!node->Arguments.empty())
					{
						for (auto argument : node->Arguments)
						{
							Visit(output, argument);

							output += ", ";
						}

						output.erase(output.end() - 2, output.end());
					}

					output += ')';

					if (node->Type.IsMatrix())
					{
						output += ')';
					}
				}
				void Visit(std::string &output, const FX::Nodes::InitializerList *node, const FX::Nodes::Type &type)
				{
					VisitTypeClass(output, type);
					
					output += "[](";

					if (!node->Values.empty())
					{
						for (auto expression : node->Values)
						{
							if (expression->NodeId == FX::Node::Id::InitializerList)
							{
								Visit(output, static_cast<FX::Nodes::InitializerList *>(expression), node->Type);
							}
							else
							{
								const auto cast = PrintCast(expression->Type, type);

								output += cast.first;
							
								Visit(output, expression);
							
								output += cast.second;
							}

							output += ", ";
						}

						output.erase(output.end() - 2, output.end());
					}

					output += ')';
				}

				void Visit(std::string &output, const FX::Nodes::Struct *node)
				{
					output += "struct " + FixName(node->Name, node->Namespace) + "\n{\n";

					if (!node->Fields.empty())
					{
						for (auto field : node->Fields)
						{
							Visit(output, field);

							output += ";\n";
						}
					}
					else
					{
						output += "float _dummy;\n";
					}

					output += "};\n";
				}
				void Visit(std::string &output, const FX::Nodes::Variable *node, bool includetype = true)
				{
					if (includetype)
					{
						VisitType(output, node->Type);
					}

					output += ' ' + FixName(node->Name, node->Namespace);

					if (node->Type.IsArray())
					{
						output += '[' + ((node->Type.ArrayLength >= 1) ? std::to_string(node->Type.ArrayLength) : "") + ']';
					}

					if (node->Initializer != nullptr)
					{
						output += " = ";

						if (node->Initializer->NodeId == FX::Node::Id::InitializerList)
						{
							Visit(output, static_cast<FX::Nodes::InitializerList *>(node->Initializer), node->Type);
						}
						else
						{
							const auto cast = PrintCast(node->Initializer->Type, node->Type);

							output += cast.first;
							
							Visit(output, node->Initializer);
							
							output += cast.second;
						}
					}
				}
				void Visit(std::string &output, const FX::Nodes::Function *node)
				{
					VisitTypeClass(output, node->ReturnType);

					output += ' ' + FixName(node->Name, node->Namespace) + '(';

					if (!node->Parameters.empty())
					{
						for (auto parameter : node->Parameters)
						{
							Visit(output, parameter);

							output += ", ";
						}

						output.erase(output.end() - 2, output.end());
					}

					output += ")\n";

					Visit(output, node->Definition);
				}

				template <typename T>
				void VisitAnnotation(const std::vector<FX::Nodes::Annotation> &annotations, T &object)
				{
					for (auto &annotation : annotations)
					{
						switch (annotation.Value->Type.BaseClass)
						{
							case FX::Nodes::Type::Class::Bool:
							case FX::Nodes::Type::Class::Int:
								object.Annotations[annotation.Name] = annotation.Value->Value.Int;
								break;
							case FX::Nodes::Type::Class::Uint:
								object.Annotations[annotation.Name] = annotation.Value->Value.Uint;
								break;
							case FX::Nodes::Type::Class::Float:
								object.Annotations[annotation.Name] = annotation.Value->Value.Float;
								break;
							case FX::Nodes::Type::Class::String:
								object.Annotations[annotation.Name] = annotation.Value->StringValue;
								break;
						}
					}
				}
				void VisitTexture(const FX::Nodes::Variable *node)
				{
					GLTexture *const obj = new GLTexture();
					obj->Name = node->Name;
					GLuint width = obj->Width = node->Properties.Width;
					GLuint height = obj->Height = node->Properties.Height;
					GLuint levels = obj->Levels = node->Properties.MipLevels;

					GLenum internalformat = GL_RGBA8, internalformatSRGB = GL_SRGB8_ALPHA8;
					LiteralToFormat(node->Properties.Format, internalformat, internalformatSRGB, obj->Format);

					if (levels == 0)
					{
						Warning(node->Location, "a texture cannot have 0 miplevels, changed it to 1");

						levels = 1;
					}

					VisitAnnotation(node->Annotations, *obj);

					if (node->Semantic == "COLOR" || node->Semantic == "SV_TARGET")
					{
						if (width != 1 || height != 1 || levels != 1 || internalformat != GL_RGBA8)
						{
							Warning(node->Location, "texture property on backbuffer textures are ignored");
						}

						obj->ChangeDataSource(GLTexture::Source::BackBuffer, _runtime->_backbufferTexture[0], _runtime->_backbufferTexture[1]);
					}
					else if (node->Semantic == "DEPTH" || node->Semantic == "SV_DEPTH")
					{
						if (width != 1 || height != 1 || levels != 1 || internalformat != GL_RGBA8)
						{
							Warning(node->Location, "texture property on depthbuffer textures are ignored");
						}

						obj->ChangeDataSource(GLTexture::Source::DepthStencil, _runtime->_depthTexture, 0);
					}
					else
					{
						obj->DataSource = GLTexture::Source::Memory;

						GLCHECK(glGenTextures(2, obj->ID));

						GLint previous = 0, previousFBO = 0;
						GLCHECK(glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous));
						GLCHECK(glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousFBO));

						GLCHECK(glBindTexture(GL_TEXTURE_2D, obj->ID[0]));
						GLCHECK(glTexStorage2D(GL_TEXTURE_2D, levels, internalformat, width, height));
						GLCHECK(glTextureView(obj->ID[1], GL_TEXTURE_2D, obj->ID[0], internalformatSRGB, 0, levels, 0, 1));
						GLCHECK(glBindTexture(GL_TEXTURE_2D, previous));

						// Clear texture to black
						GLCHECK(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _runtime->_blitFBO));
						GLCHECK(glFramebufferTexture(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, obj->ID[0], 0));
						GLCHECK(glDrawBuffer(GL_COLOR_ATTACHMENT1));
						const GLuint clearColor[4] = { 0, 0, 0, 0 };
						GLCHECK(glClearBufferuiv(GL_COLOR, 0, clearColor));
						GLCHECK(glFramebufferTexture(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, 0, 0));
						GLCHECK(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previousFBO));
					}

					_runtime->AddTexture(obj);
				}
				void VisitSampler(const FX::Nodes::Variable *node)
				{
					if (node->Properties.Texture == nullptr)
					{
						Error(node->Location, "sampler '%s' is missing required 'Texture' property", node->Name.c_str());
						return;
					}

					GLTexture *const texture = static_cast<GLTexture *>(_runtime->GetTexture(node->Properties.Texture->Name));

					if (texture == nullptr)
					{
						_isFatal = true;
						return;
					}

					GLSampler sampler;
					sampler.ID = 0;
					sampler.Texture = texture;
					sampler.SRGB = node->Properties.SRGBTexture;

					GLenum minfilter = LiteralToTextureFilter(node->Properties.MinFilter);
					const GLenum mipfilter = LiteralToTextureFilter(node->Properties.MipFilter);
				
					if (minfilter == GL_NEAREST && mipfilter == GL_NEAREST)
					{
						minfilter = GL_NEAREST_MIPMAP_NEAREST;
					}
					else if (minfilter == GL_NEAREST && mipfilter == GL_LINEAR)
					{
						minfilter = GL_NEAREST_MIPMAP_LINEAR;
					}
					else if (minfilter == GL_LINEAR && mipfilter == GL_NEAREST)
					{
						minfilter = GL_LINEAR_MIPMAP_NEAREST;
					}
					else if (minfilter == GL_LINEAR && mipfilter == GL_LINEAR)
					{
						minfilter = GL_LINEAR_MIPMAP_LINEAR;
					}

					GLCHECK(glGenSamplers(1, &sampler.ID));
					GLCHECK(glSamplerParameteri(sampler.ID, GL_TEXTURE_WRAP_S, LiteralToTextureWrap(node->Properties.AddressU)));
					GLCHECK(glSamplerParameteri(sampler.ID, GL_TEXTURE_WRAP_T, LiteralToTextureWrap(node->Properties.AddressV)));
					GLCHECK(glSamplerParameteri(sampler.ID, GL_TEXTURE_WRAP_R, LiteralToTextureWrap(node->Properties.AddressW)));
					GLCHECK(glSamplerParameteri(sampler.ID, GL_TEXTURE_MIN_FILTER, minfilter));
					GLCHECK(glSamplerParameteri(sampler.ID, GL_TEXTURE_MAG_FILTER, LiteralToTextureFilter(node->Properties.MagFilter)));
					GLCHECK(glSamplerParameterf(sampler.ID, GL_TEXTURE_LOD_BIAS, node->Properties.MipLODBias));
					GLCHECK(glSamplerParameterf(sampler.ID, GL_TEXTURE_MIN_LOD, node->Properties.MinLOD));
					GLCHECK(glSamplerParameterf(sampler.ID, GL_TEXTURE_MAX_LOD, node->Properties.MaxLOD));
					GLCHECK(glSamplerParameterf(sampler.ID, GL_TEXTURE_MAX_ANISOTROPY_EXT, static_cast<GLfloat>(node->Properties.MaxAnisotropy)));

					_globalCode += "layout(binding = " + std::to_string(_runtime->_effectSamplers.size()) + ") uniform sampler2D ";
					_globalCode += FixName(node->Name, node->Namespace);
					_globalCode += ";\n";

					_runtime->_effectSamplers.push_back(std::move(sampler));
				}
				void VisitUniform(const FX::Nodes::Variable *node)
				{
					VisitType(_globalUniforms, node->Type);

					_globalUniforms += ' ';			
					_globalUniforms += FixName(node->Name, node->Namespace);

					if (node->Type.IsArray())
					{
						_globalUniforms += '[' + ((node->Type.ArrayLength >= 1) ? std::to_string(node->Type.ArrayLength) : "") + ']';
					}

					_globalUniforms += ";\n";

					Uniform *const obj = new Uniform();
					obj->Name = node->Name;
					obj->Rows = node->Type.Rows;
					obj->Columns = node->Type.Cols;
					obj->Elements = node->Type.ArrayLength;
					obj->StorageSize = obj->Rows * obj->Columns * std::max(1u, obj->Elements);

					switch (node->Type.BaseClass)
					{
						case FX::Nodes::Type::Class::Bool:
							obj->BaseType = Uniform::Type::Bool;
							obj->StorageSize *= sizeof(int);
							break;
						case FX::Nodes::Type::Class::Int:
							obj->BaseType = Uniform::Type::Int;
							obj->StorageSize *= sizeof(int);
							break;
						case FX::Nodes::Type::Class::Uint:
							obj->BaseType = Uniform::Type::Uint;
							obj->StorageSize *= sizeof(unsigned int);
							break;
						case FX::Nodes::Type::Class::Float:
							obj->BaseType = Uniform::Type::Float;
							obj->StorageSize *= sizeof(float);
							break;
					}

					const size_t alignment = 16 - (_currentGlobalSize % 16);
					_currentGlobalSize += (obj->StorageSize > alignment && (alignment != 16 || obj->StorageSize <= 16)) ? obj->StorageSize + alignment : obj->StorageSize;
					obj->StorageOffset = _currentGlobalSize - obj->StorageSize;

					VisitAnnotation(node->Annotations, *obj);

					if (_currentGlobalSize >= _runtime->GetConstantStorageSize())
					{
						_runtime->EnlargeConstantStorage();
					}

					if (node->Initializer != nullptr && node->Initializer->NodeId == FX::Node::Id::Literal)
					{
						std::memcpy(_runtime->GetConstantStorage() + obj->StorageOffset, &static_cast<const FX::Nodes::Literal *>(node->Initializer)->Value, obj->StorageSize);
					}
					else
					{
						std::memset(_runtime->GetConstantStorage() + obj->StorageOffset, 0, obj->StorageSize);
					}

					_runtime->AddConstant(obj);
				}
				void VisitTechnique(const FX::Nodes::Technique *node)
				{
					GLTechnique *const obj = new GLTechnique();
					obj->Name = node->Name;
					obj->PassCount = static_cast<unsigned int>(node->Passes.size());

					VisitAnnotation(node->Annotations, *obj);

					for (auto pass : node->Passes)
					{
						VisitTechniquePass(pass, obj->Passes);
					}

					_runtime->AddTechnique(obj);
				}
				void VisitTechniquePass(const FX::Nodes::Pass *node, std::vector<GLTechnique::Pass> &passes)
				{
					GLTechnique::Pass pass;
					ZeroMemory(&pass, sizeof(GLTechnique::Pass));
					pass.ColorMaskR = (node->States.RenderTargetWriteMask & (1 << 0)) != 0;
					pass.ColorMaskG = (node->States.RenderTargetWriteMask & (1 << 1)) != 0;
					pass.ColorMaskB = (node->States.RenderTargetWriteMask & (1 << 2)) != 0;
					pass.ColorMaskA = (node->States.RenderTargetWriteMask & (1 << 3)) != 0;
					pass.DepthTest = node->States.DepthEnable;
					pass.DepthMask = node->States.DepthWriteMask;
					pass.DepthFunc = LiteralToCompFunc(node->States.DepthFunc);
					pass.StencilTest = node->States.StencilEnable;
					pass.StencilReadMask = node->States.StencilReadMask;
					pass.StencilMask = node->States.StencilWriteMask;
					pass.StencilFunc = LiteralToCompFunc(node->States.StencilFunc);
					pass.StencilOpZPass = LiteralToStencilOp(node->States.StencilOpPass);
					pass.StencilOpFail = LiteralToStencilOp(node->States.StencilOpFail);
					pass.StencilOpZFail = LiteralToStencilOp(node->States.StencilOpDepthFail);
					pass.Blend = node->States.BlendEnable;
					pass.BlendEqColor = LiteralToBlendEq(node->States.BlendOp);
					pass.BlendEqAlpha = LiteralToBlendEq(node->States.BlendOpAlpha);
					pass.BlendFuncSrc = LiteralToBlendFunc(node->States.SrcBlend);
					pass.BlendFuncDest = LiteralToBlendFunc(node->States.DestBlend);
					pass.StencilRef = node->States.StencilRef;
					pass.FramebufferSRGB = node->States.SRGBWriteEnable;

					GLCHECK(glGenFramebuffers(1, &pass.Framebuffer));
					GLCHECK(glBindFramebuffer(GL_FRAMEBUFFER, pass.Framebuffer));

					bool backbufferFramebuffer = true;

					for (unsigned int i = 0; i < 8; ++i)
					{
						if (node->States.RenderTargets[i] == nullptr)
						{
							continue;
						}

						const GLTexture *const texture = static_cast<GLTexture *>(_runtime->GetTexture(node->States.RenderTargets[i]->Name));

						if (texture == nullptr)
						{
							_isFatal = true;
							return;
						}

						if (pass.ViewportWidth != 0 && pass.ViewportHeight != 0 && (texture->Width != static_cast<unsigned int>(pass.ViewportWidth) || texture->Height != static_cast<unsigned int>(pass.ViewportHeight)))
						{
							Error(node->Location, "cannot use multiple rendertargets with different sized textures");
							return;
						}
						else
						{
							pass.ViewportWidth = texture->Width;
							pass.ViewportHeight = texture->Height;
						}

						backbufferFramebuffer = false;

						GLCHECK(glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, texture->ID[pass.FramebufferSRGB], 0));

						pass.DrawBuffers[i] = GL_COLOR_ATTACHMENT0 + i;
						pass.DrawTextures[i] = texture->ID[pass.FramebufferSRGB];
					}

					if (backbufferFramebuffer)
					{
						GLCHECK(glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, _runtime->_defaultBackBufferRBO[0]));

						pass.DrawBuffers[0] = GL_COLOR_ATTACHMENT0;
						pass.DrawTextures[0] = _runtime->_backbufferTexture[1];

						RECT rect;
						GetClientRect(WindowFromDC(_runtime->_hdc), &rect);

						pass.ViewportWidth = static_cast<GLsizei>(rect.right - rect.left);
						pass.ViewportHeight = static_cast<GLsizei>(rect.bottom - rect.top);
					}

					GLCHECK(glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, _runtime->_defaultBackBufferRBO[1]));

					assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

					GLCHECK(glBindFramebuffer(GL_FRAMEBUFFER, 0));

					GLuint shaders[2] = { 0, 0 };
					GLenum shaderTypes[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
					const FX::Nodes::Function *shaderFunctions[2] = { node->States.VertexShader, node->States.PixelShader };

					pass.Program = glCreateProgram();

					for (unsigned int i = 0; i < 2; ++i)
					{
						if (shaderFunctions[i] != nullptr)
						{
							shaders[i] = glCreateShader(shaderTypes[i]);

							VisitTechniquePassShader(shaderFunctions[i], shaderTypes[i], shaders[i]);

							GLCHECK(glAttachShader(pass.Program, shaders[i]));
						}
					}

					GLCHECK(glLinkProgram(pass.Program));

					for (unsigned int i = 0; i < 2; ++i)
					{
						GLCHECK(glDetachShader(pass.Program, shaders[i]));
						GLCHECK(glDeleteShader(shaders[i]));
					}

					GLint status = GL_FALSE;

					GLCHECK(glGetProgramiv(pass.Program, GL_LINK_STATUS, &status));

					if (status == GL_FALSE)
					{
						GLint logsize = 0;
						GLCHECK(glGetProgramiv(pass.Program, GL_INFO_LOG_LENGTH, &logsize));

						std::string log(logsize, '\0');
						GLCHECK(glGetProgramInfoLog(pass.Program, logsize, nullptr, &log.front()));

						GLCHECK(glDeleteProgram(pass.Program));

						_errors += log;
						_isFatal = true;
						return;
					}

					passes.push_back(std::move(pass));
				}
				void VisitTechniquePassShader(const FX::Nodes::Function *node, GLuint shadertype, GLuint &shader)
				{
					std::string source =
						"#version 430\n"
						"float _fmod(float x, float y) { return x - y * trunc(x / y); }"
						"vec2 _fmod(vec2 x, vec2 y) { return x - y * trunc(x / y); }"
						"vec3 _fmod(vec3 x, vec3 y) { return x - y * trunc(x / y); }"
						"vec4 _fmod(vec4 x, vec4 y) { return x - y * trunc(x / y); }"
						"mat2 _fmod(mat2 x, mat2 y) { return x - matrixCompMult(y, mat2(trunc(x[0] / y[0]), trunc(x[1] / y[1]))); }"
						"mat3 _fmod(mat3 x, mat3 y) { return x - matrixCompMult(y, mat3(trunc(x[0] / y[0]), trunc(x[1] / y[1]), trunc(x[2] / y[2]))); }"
						"mat4 _fmod(mat4 x, mat4 y) { return x - matrixCompMult(y, mat4(trunc(x[0] / y[0]), trunc(x[1] / y[1]), trunc(x[2] / y[2]), trunc(x[3] / y[3]))); }\n"
						"void _sincos(float x, out float s, out float c) { s = sin(x), c = cos(x); }"
						"void _sincos(vec2 x, out vec2 s, out vec2 c) { s = sin(x), c = cos(x); }"
						"void _sincos(vec3 x, out vec3 s, out vec3 c) { s = sin(x), c = cos(x); }"
						"void _sincos(vec4 x, out vec4 s, out vec4 c) { s = sin(x), c = cos(x); }\n"
						"vec4 _textureLod(sampler2D s, vec4 c) { return textureLod(s, c.xy, c.w); }\n"
						"#define _textureLodOffset(s, c, offset) textureLodOffset(s, (c).xy, (c).w, offset)\n";

					if (!_globalUniforms.empty())
					{
						source += "layout(std140, binding = 0) uniform _GLOBAL_\n{\n" + _globalUniforms + "};\n";
					}

					if (shadertype != GL_FRAGMENT_SHADER)
					{
						source += "#define discard\n";
					}

					source += _globalCode;

					for (auto dependency : _functions.at(node).FunctionDependencies)
					{
						source += _functions.at(dependency).SourceCode;
					}

					source += _functions.at(node).SourceCode;

					for (auto parameter : node->Parameters)
					{
						if (parameter->Type.IsStruct())
						{
							for (auto field : parameter->Type.Definition->Fields)
							{
								VisitShaderParameter(source, field->Type, parameter->Type.Qualifiers, "_param_" + parameter->Name + "_" + field->Name, field->Semantic, shadertype);
							}
						}
						else
						{
							VisitShaderParameter(source, parameter->Type, parameter->Type.Qualifiers, "_param_" + parameter->Name, parameter->Semantic, shadertype);
						}
					}

					if (node->ReturnType.IsStruct())
					{
						for (auto field : node->ReturnType.Definition->Fields)
						{
							VisitShaderParameter(source, field->Type, FX::Nodes::Type::Out, "_return_" + field->Name, field->Semantic, shadertype);
						}
					}
					else if (!node->ReturnType.IsVoid())
					{
						VisitShaderParameter(source, node->ReturnType, FX::Nodes::Type::Out, "_return", node->ReturnSemantic, shadertype);
					}

					source += "void main()\n{\n";

					for (auto parameter : node->Parameters)
					{
						for (int i = 0, arraylength = std::max(1, parameter->Type.ArrayLength); i < arraylength; ++i)
						{
							if (parameter->Type.IsStruct())
							{
								VisitTypeClass(source, parameter->Type);

								source += " _param_" + parameter->Name + (parameter->Type.IsArray() ? std::to_string(i) : "") + " = ";

								VisitTypeClass(source, parameter->Type);

								source += "(";

								if (!parameter->Type.Definition->Fields.empty())
								{
									for (auto field : parameter->Type.Definition->Fields)
									{
										source += FixNameWithSemantic("_param_" + parameter->Name + "_" + field->Name + (parameter->Type.IsArray() ? std::to_string(i) : ""), field->Semantic, shadertype) + ", ";
									}

									source.erase(source.end() - 2, source.end());
								}

								source += ");\n";
							}
							else if (boost::starts_with(parameter->Semantic, "COLOR") || boost::starts_with(parameter->Semantic, "SV_TARGET"))
							{
								source += " _param_" + parameter->Name + (parameter->Type.IsArray() ? std::to_string(i) : "") + " = vec4(0, 0, 0, 1);\n";
							}
						}

						if (parameter->Type.IsArray())
						{
							VisitTypeClass(source, parameter->Type);

							source += " _param_" + parameter->Name + "[] = ";

							VisitTypeClass(source, parameter->Type);

							source += "[](";

							for (int i = 0; i < parameter->Type.ArrayLength; ++i)
							{
								source += "_param_" + parameter->Name + std::to_string(i) + ", ";
							}

							source.erase(source.end() - 2, source.end());

							source += ");\n";
						}
					}

					if (node->ReturnType.IsStruct())
					{
						VisitTypeClass(source, node->ReturnType);

						source += ' ';
					}

					if (!node->ReturnType.IsVoid())
					{
						source += "_return = ";

						if ((boost::starts_with(node->ReturnSemantic, "COLOR") || boost::starts_with(node->ReturnSemantic, "SV_TARGET")) && node->ReturnType.Rows < 4)
						{
							const std::string swizzle[3] = { "x", "xy", "xyz" };

							source += "vec4(0, 0, 0, 1);\n_return." + swizzle[node->ReturnType.Rows - 1] + " = ";
						}
					}

					source += FixName(node->Name, node->Namespace) + '(';

					if (!node->Parameters.empty())
					{
						for (auto parameter : node->Parameters)
						{
							source += FixNameWithSemantic("_param_" + parameter->Name, parameter->Semantic, shadertype);

							if ((boost::starts_with(parameter->Semantic, "COLOR") || boost::starts_with(parameter->Semantic, "SV_TARGET")) && parameter->Type.Rows < 4)
							{
								const std::string swizzle[3] = { "x", "xy", "xyz" };

								source += "." + swizzle[parameter->Type.Rows - 1];
							}

							source += ", ";
						}

						source.erase(source.end() - 2, source.end());
					}

					source += ");\n";

					for (auto parameter : node->Parameters)
					{
						if (!parameter->Type.HasQualifier(FX::Nodes::Type::Qualifier::Out))
						{
							continue;
						}

						if (parameter->Type.IsArray())
						{
							for (int i = 0; i < parameter->Type.ArrayLength; ++i)
							{
								source += "_param_" + parameter->Name + std::to_string(i) + " = _param_" + parameter->Name + "[" + std::to_string(i) + "];\n";
							}
						}

						for (int i = 0; i < std::max(1, parameter->Type.ArrayLength); ++i)
						{
							if (parameter->Type.IsStruct())
							{
								for (auto field : parameter->Type.Definition->Fields)
								{
									source += "_param_" + parameter->Name + "_" + field->Name + (parameter->Type.IsArray() ? std::to_string(i) : "") + " = " + "_param_" + parameter->Name + "." + field->Name + (parameter->Type.IsArray() ? "[" + std::to_string(i) + "]" : "") + ";\n";
								}
							}
						}
					}

					if (node->ReturnType.IsStruct())
					{
						for (auto field : node->ReturnType.Definition->Fields)
						{
							source += FixNameWithSemantic("_return_" + field->Name, field->Semantic, shadertype) + " = _return." + field->Name + ";\n";
						}
					}
			
					if (shadertype == GL_VERTEX_SHADER)
					{
						source += "gl_Position = gl_Position * vec4(1.0, 1.0, 2.0, 1.0) + vec4(0.0, 0.0, -gl_Position.w, 0.0);\n";
					}

					source += "}\n";

					LOG(TRACE) << "> Compiling shader '" << node->Name << "':\n\n" << source.c_str() << "\n";

					GLint status = GL_FALSE;
					const GLchar *src = source.c_str();
					const GLsizei len = static_cast<GLsizei>(source.length());

					GLCHECK(glShaderSource(shader, 1, &src, &len));
					GLCHECK(glCompileShader(shader));
					GLCHECK(glGetShaderiv(shader, GL_COMPILE_STATUS, &status));

					if (status == GL_FALSE)
					{
						GLint logsize = 0;
						GLCHECK(glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logsize));

						std::string log(logsize, '\0');
						GLCHECK(glGetShaderInfoLog(shader, logsize, nullptr, &log.front()));

						_errors += log;
						_isFatal = true;
					}
				}
				void VisitShaderParameter(std::string &source, FX::Nodes::Type type, unsigned int qualifier, const std::string &name, const std::string &semantic, GLuint shadertype)
				{
					type.Qualifiers = static_cast<unsigned int>(qualifier);

					unsigned int location = 0;

					for (int i = 0, arraylength = std::max(1, type.ArrayLength); i < arraylength; ++i)
					{
						if (!FixNameWithSemantic(std::string(), semantic, shadertype).empty())
						{
							continue;
						}
						else if (boost::starts_with(semantic, "COLOR"))
						{
							type.Rows = 4;

							location = static_cast<unsigned int>(::strtol(semantic.c_str() + 5, nullptr, 10));
						}
						else if (boost::starts_with(semantic, "TEXCOORD"))
						{
							location = static_cast<unsigned int>(::strtol(semantic.c_str() + 8, nullptr, 10)) + 1;
						}
						else if (boost::starts_with(semantic, "SV_TARGET"))
						{
							type.Rows = 4;

							location = static_cast<unsigned int>(::strtol(semantic.c_str() + 9, nullptr, 10));
						}

						source += "layout(location = " + std::to_string(location + i) + ") ";

						VisitType(source, type);

						source += ' ' + name;

						if (type.IsArray())
						{
							source += std::to_string(i);
						}

						source += ";\n";
					}
				}

			private:
				struct Function
				{
					std::string SourceCode;
					std::vector<const FX::Nodes::Function *> FunctionDependencies;
				};

				GLRuntime *_runtime;
				const FX::NodeTree &_ast;
				bool _isFatal;
				std::string _errors;
				std::string _globalCode, _globalUniforms;
				size_t _currentGlobalSize;
				const FX::Nodes::Function *_currentFunction;
				std::unordered_map<const FX::Nodes::Function *, Function> _functions;
			};

			GLenum TargetToBinding(GLenum target)
			{
				switch (target)
				{
					case GL_FRAMEBUFFER:
						return GL_FRAMEBUFFER_BINDING;
					case GL_READ_FRAMEBUFFER:
						return GL_READ_FRAMEBUFFER_BINDING;
					case GL_DRAW_FRAMEBUFFER:
						return GL_DRAW_FRAMEBUFFER_BINDING;
					case GL_RENDERBUFFER:
						return GL_RENDERBUFFER_BINDING;
					case GL_TEXTURE_2D:
						return GL_TEXTURE_BINDING_2D;
					case GL_TEXTURE_2D_ARRAY:
						return GL_TEXTURE_BINDING_2D_ARRAY;
					case GL_TEXTURE_2D_MULTISAMPLE:
						return GL_TEXTURE_BINDING_2D_MULTISAMPLE;
					case GL_TEXTURE_2D_MULTISAMPLE_ARRAY:
						return GL_TEXTURE_BINDING_2D_MULTISAMPLE_ARRAY;
					case GL_TEXTURE_3D:
						return GL_TEXTURE_BINDING_3D;
					case GL_TEXTURE_CUBE_MAP:
					case GL_TEXTURE_CUBE_MAP_NEGATIVE_X:
					case GL_TEXTURE_CUBE_MAP_NEGATIVE_Y:
					case GL_TEXTURE_CUBE_MAP_NEGATIVE_Z:
					case GL_TEXTURE_CUBE_MAP_POSITIVE_X:
					case GL_TEXTURE_CUBE_MAP_POSITIVE_Y:
					case GL_TEXTURE_CUBE_MAP_POSITIVE_Z:
						return GL_TEXTURE_BINDING_CUBE_MAP;
					case GL_TEXTURE_CUBE_MAP_ARRAY:
						return GL_TEXTURE_BINDING_CUBE_MAP_ARRAY;
					default:
						return GL_NONE;
				}
			}
			inline void FlipBC1Block(unsigned char *block)
			{
				// BC1 Block:
				//  [0-1]  color 0
				//  [2-3]  color 1
				//  [4-7]  color indices

				std::swap(block[4], block[7]);
				std::swap(block[5], block[6]);
			}
			inline void FlipBC2Block(unsigned char *block)
			{
				// BC2 Block:
				//  [0-7]  alpha indices
				//  [8-15] color block

				std::swap(block[0], block[6]);
				std::swap(block[1], block[7]);
				std::swap(block[2], block[4]);
				std::swap(block[3], block[5]);

				FlipBC1Block(block + 8);
			}
			inline void FlipBC4Block(unsigned char *block)
			{
				// BC4 Block:
				//  [0]    red 0
				//  [1]    red 1
				//  [2-7]  red indices

				const unsigned int line_0_1 = block[2] + 256 * (block[3] + 256 * block[4]);
				const unsigned int line_2_3 = block[5] + 256 * (block[6] + 256 * block[7]);
				const unsigned int line_1_0 = ((line_0_1 & 0x000FFF) << 12) | ((line_0_1 & 0xFFF000) >> 12);
				const unsigned int line_3_2 = ((line_2_3 & 0x000FFF) << 12) | ((line_2_3 & 0xFFF000) >> 12);
				block[2] = static_cast<unsigned char>((line_3_2 & 0xFF));
				block[3] = static_cast<unsigned char>((line_3_2 & 0xFF00) >> 8);
				block[4] = static_cast<unsigned char>((line_3_2 & 0xFF0000) >> 16);
				block[5] = static_cast<unsigned char>((line_1_0 & 0xFF));
				block[6] = static_cast<unsigned char>((line_1_0 & 0xFF00) >> 8);
				block[7] = static_cast<unsigned char>((line_1_0 & 0xFF0000) >> 16);
			}
			inline void FlipBC3Block(unsigned char *block)
			{
				// BC3 Block:
				//  [0-7]  alpha block
				//  [8-15] color block

				FlipBC4Block(block);
				FlipBC1Block(block + 8);
			}
			inline void FlipBC5Block(unsigned char *block)
			{
				// BC5 Block:
				//  [0-7]  red block
				//  [8-15] green block

				FlipBC4Block(block);
				FlipBC4Block(block + 8);
			}
			void FlipImageData(const Texture *texture, unsigned char *data)
			{
				typedef void (*FlipBlockFunc)(unsigned char *block);

				size_t blocksize = 0;
				bool compressed = false;
				FlipBlockFunc compressedFunc = nullptr;

				switch (texture->Format)
				{
					case Texture::PixelFormat::R8:
						blocksize = 1;
						break;
					case Texture::PixelFormat::RG8:
						blocksize = 2;
						break;
					case Texture::PixelFormat::R32F:
					case Texture::PixelFormat::RGBA8:
						blocksize = 4;
						break;
					case Texture::PixelFormat::RGBA16:
					case Texture::PixelFormat::RGBA16F:
						blocksize = 8;
						break;
					case Texture::PixelFormat::RGBA32F:
						blocksize = 16;
						break;
					case Texture::PixelFormat::DXT1:
						blocksize = 8;
						compressed = true;
						compressedFunc = &FlipBC1Block;
						break;
					case Texture::PixelFormat::DXT3:
						blocksize = 16;
						compressed = true;
						compressedFunc = &FlipBC2Block;
						break;
					case Texture::PixelFormat::DXT5:
						blocksize = 16;
						compressed = true;
						compressedFunc = &FlipBC3Block;
						break;
					case Texture::PixelFormat::LATC1:
						blocksize = 8;
						compressed = true;
						compressedFunc = &FlipBC4Block;
						break;
					case Texture::PixelFormat::LATC2:
						blocksize = 16;
						compressed = true;
						compressedFunc = &FlipBC5Block;
						break;
					default:
						return;
				}

				if (compressed)
				{
					const size_t w = (texture->Width + 3) / 4;
					const size_t h = (texture->Height + 3) / 4;
					const size_t stride = w * blocksize;

					for (size_t y = 0; y < h; ++y)
					{
						unsigned char *dataLine = data + stride * (h - 1 - y);

						for (size_t x = 0; x < stride; x += blocksize)
						{
							compressedFunc(dataLine + x);
						}
					}
				}
				else
				{
					const size_t w = texture->Width;
					const size_t h = texture->Height;
					const size_t stride = w * blocksize;
					unsigned char *templine = static_cast<unsigned char *>(::alloca(stride));

					for (size_t y = 0; 2 * y < h; ++y)
					{
						unsigned char *line1 = data + stride * y;
						unsigned char *line2 = data + stride * (h - 1 - y);

						std::memcpy(templine, line1, stride);
						std::memcpy(line1, line2, stride);
						std::memcpy(line2, templine, stride);
					}
				}
			}

			unsigned int GetRendererId()
			{
				GLint major = 0, minor = 0;
				GLCHECK(glGetIntegerv(GL_MAJOR_VERSION, &major));
				GLCHECK(glGetIntegerv(GL_MAJOR_VERSION, &minor));

				return 0x10000 | (major << 12) | (minor << 8);
			}
		}

		class GLStateBlock
		{
		public:
			GLStateBlock()
			{
				ZeroMemory(this, sizeof(*this));
			}

			void Capture()
			{
				GLCHECK(glGetIntegerv(GL_VIEWPORT, _viewport));
				GLCHECK(_stencilTest = glIsEnabled(GL_STENCIL_TEST));
				GLCHECK(_scissorTest = glIsEnabled(GL_SCISSOR_TEST));
				GLCHECK(glGetIntegerv(GL_FRONT_FACE, reinterpret_cast<GLint *>(&_frontFace)));
				GLCHECK(glGetIntegerv(GL_POLYGON_MODE, reinterpret_cast<GLint *>(&_polygonMode)));
				GLCHECK(_cullFace = glIsEnabled(GL_CULL_FACE));
				GLCHECK(glGetIntegerv(GL_CULL_FACE_MODE, reinterpret_cast<GLint *>(&_cullFaceMode)));
				GLCHECK(glGetBooleanv(GL_COLOR_WRITEMASK, _colorMask));
				GLCHECK(_framebufferSRGB = glIsEnabled(GL_FRAMEBUFFER_SRGB));
				GLCHECK(_blend = glIsEnabled(GL_BLEND));
				GLCHECK(glGetIntegerv(GL_BLEND_SRC, reinterpret_cast<GLint *>(&_blendFuncSrc)));
				GLCHECK(glGetIntegerv(GL_BLEND_DST, reinterpret_cast<GLint *>(&_blendFuncDest)));
				GLCHECK(glGetIntegerv(GL_BLEND_EQUATION_RGB, reinterpret_cast<GLint *>(&_blendEqColor)));
				GLCHECK(glGetIntegerv(GL_BLEND_EQUATION_ALPHA, reinterpret_cast<GLint *>(&_blendEqAlpha)));
				GLCHECK(_depthTest = glIsEnabled(GL_DEPTH_TEST));
				GLCHECK(glGetBooleanv(GL_DEPTH_WRITEMASK, &_depthMask));
				GLCHECK(glGetIntegerv(GL_DEPTH_FUNC, reinterpret_cast<GLint *>(&_depthFunc)));
				GLCHECK(glGetIntegerv(GL_STENCIL_VALUE_MASK, reinterpret_cast<GLint *>(&_stencilReadMask)));
				GLCHECK(glGetIntegerv(GL_STENCIL_WRITEMASK, reinterpret_cast<GLint *>(&_stencilMask)));
				GLCHECK(glGetIntegerv(GL_STENCIL_FUNC, reinterpret_cast<GLint *>(&_stencilFunc)));
				GLCHECK(glGetIntegerv(GL_STENCIL_FAIL, reinterpret_cast<GLint *>(&_stencilOpFail)));
				GLCHECK(glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL, reinterpret_cast<GLint *>(&_stencilOpZFail)));
				GLCHECK(glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, reinterpret_cast<GLint *>(&_stencilOpZPass)));
				GLCHECK(glGetIntegerv(GL_STENCIL_REF, &_stencilRef));
				GLCHECK(glGetIntegerv(GL_ACTIVE_TEXTURE, reinterpret_cast<GLint *>(&_activeTexture)));

				for (GLuint i = 0; i < 8; ++i)
				{
					glGetIntegerv(GL_DRAW_BUFFER0 + i, reinterpret_cast<GLint *>(&_drawBuffers[i]));
				}

				GLCHECK(glGetIntegerv(GL_CURRENT_PROGRAM, reinterpret_cast<GLint *>(&_program)));
				GLCHECK(glGetIntegerv(GL_FRAMEBUFFER_BINDING, reinterpret_cast<GLint *>(&_fbo)));
				GLCHECK(glGetIntegerv(GL_VERTEX_ARRAY_BINDING, reinterpret_cast<GLint *>(&_vao)));
				GLCHECK(glGetIntegerv(GL_ARRAY_BUFFER_BINDING, reinterpret_cast<GLint *>(&_vbo)));
				GLCHECK(glGetIntegerv(GL_UNIFORM_BUFFER_BINDING, reinterpret_cast<GLint *>(&_ubo)));
				
				for (GLuint i = 0; i < ARRAYSIZE(_textures2D); ++i)
				{
					glActiveTexture(GL_TEXTURE0 + i);
					glGetIntegerv(GL_TEXTURE_BINDING_2D, reinterpret_cast<GLint *>(&_textures2D[i]));
					glGetIntegerv(GL_SAMPLER_BINDING, reinterpret_cast<GLint *>(&_samplers[i]));
				}
			}
			void Apply() const
			{
				GLCHECK(glUseProgram(glIsProgram(_program) ? _program : 0));
				GLCHECK(glBindFramebuffer(GL_FRAMEBUFFER, glIsFramebuffer(_fbo) ? _fbo : 0));
				GLCHECK(glBindVertexArray(glIsVertexArray(_vao) ? _vao : 0));
				GLCHECK(glBindBuffer(GL_ARRAY_BUFFER, glIsBuffer(_vbo) ? _vbo : 0));
				GLCHECK(glBindBuffer(GL_UNIFORM_BUFFER, glIsBuffer(_ubo) ? _ubo : 0));

				for (GLuint i = 0; i < ARRAYSIZE(_textures2D); ++i)
				{
					GLCHECK(glActiveTexture(GL_TEXTURE0 + i));
					GLCHECK(glBindTexture(GL_TEXTURE_2D, glIsTexture(_textures2D[i]) ? _textures2D[i] : 0));
					GLCHECK(glBindSampler(i, glIsSampler(_samplers[i]) ? _samplers[i] : 0));
				}

	#define glEnableb(cap, value) if ((value)) glEnable(cap); else glDisable(cap);

				GLCHECK(glViewport(_viewport[0], _viewport[1], _viewport[2], _viewport[3]));
				GLCHECK(glEnableb(GL_STENCIL_TEST, _stencilTest));
				GLCHECK(glEnableb(GL_SCISSOR_TEST, _scissorTest));
				GLCHECK(glFrontFace(_frontFace));
				GLCHECK(glPolygonMode(GL_FRONT_AND_BACK, _polygonMode));
				GLCHECK(glEnableb(GL_CULL_FACE, _cullFace));
				GLCHECK(glCullFace(_cullFaceMode));
				GLCHECK(glColorMask(_colorMask[0], _colorMask[1], _colorMask[2], _colorMask[3]));
				GLCHECK(glEnableb(GL_FRAMEBUFFER_SRGB, _framebufferSRGB));
				GLCHECK(glEnableb(GL_BLEND, _blend));
				GLCHECK(glBlendFunc(_blendFuncSrc, _blendFuncDest));
				GLCHECK(glBlendEquationSeparate(_blendEqColor, _blendEqAlpha));
				GLCHECK(glEnableb(GL_DEPTH_TEST, _depthTest));
				GLCHECK(glDepthMask(_depthMask));
				GLCHECK(glDepthFunc(_depthFunc));
				GLCHECK(glStencilMask(_stencilMask));
				GLCHECK(glStencilFunc(_stencilFunc, _stencilRef, _stencilReadMask));
				GLCHECK(glStencilOp(_stencilOpFail, _stencilOpZFail, _stencilOpZPass));
				GLCHECK(glActiveTexture(_activeTexture));

				if (_drawBuffers[1] == GL_NONE &&
					_drawBuffers[2] == GL_NONE &&
					_drawBuffers[3] == GL_NONE &&
					_drawBuffers[4] == GL_NONE &&
					_drawBuffers[5] == GL_NONE &&
					_drawBuffers[6] == GL_NONE &&
					_drawBuffers[7] == GL_NONE)
				{
					glDrawBuffer(_drawBuffers[0]);
				}
				else
				{
					glDrawBuffers(8, _drawBuffers);
				}
			}

		private:
			GLint _stencilRef, _viewport[4];
			GLuint _stencilMask, _stencilReadMask;
			GLuint _program, _fbo, _vao, _vbo, _ubo, _textures2D[8], _samplers[8];
			GLenum _drawBuffers[8], _cullFace, _cullFaceMode, _polygonMode, _blendEqColor, _blendEqAlpha, _blendFuncSrc, _blendFuncDest, _depthFunc, _stencilFunc, _stencilOpFail, _stencilOpZFail, _stencilOpZPass, _frontFace, _activeTexture;
			GLboolean _scissorTest, _blend, _depthTest, _depthMask, _stencilTest, _colorMask[4], _framebufferSRGB;
		};

		// ---------------------------------------------------------------------------------------------------

		GLRuntime::GLRuntime(HDC device) : Runtime(GetRendererId()), _hdc(device), _referenceCount(1), _stateBlock(new GLStateBlock), _defaultBackBufferFBO(0), _defaultBackBufferRBO(), _backbufferTexture(), _depthSourceFBO(0), _depthSource(0), _depthTexture(0), _blitFBO(0), _defaultVAO(0), _defaultVBO(0), _effectUBO(0)
		{
			_vendorId = 0;
			_deviceId = 0;

			// Get vendor and device information on NVIDIA Optimus devices
			if (GetModuleHandleA("nvd3d9wrap.dll") == nullptr && GetModuleHandleA("nvd3d9wrapx.dll") == nullptr)
			{
				DISPLAY_DEVICEA dd;
				dd.cb = sizeof(DISPLAY_DEVICEA);

				for (DWORD i = 0; EnumDisplayDevicesA(nullptr, i, &dd, 0) != FALSE; ++i)
				{
					if ((dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0)
					{
						const std::string id = dd.DeviceID;

						if (id.length() > 20)
						{
							_vendorId = std::stoi(id.substr(8, 4), nullptr, 16);
							_deviceId = std::stoi(id.substr(17, 4), nullptr, 16);
						}
						break;
					}
				}
			}

			// Get vendor and device information on general devices
			if (_vendorId == 0)
			{
				const char *const name = reinterpret_cast<const char *>(glGetString(GL_VENDOR));

				if (name != nullptr)
				{
					if (boost::contains(name, "NVIDIA"))
					{
						_vendorId = 0x10DE;
					}
					else if (boost::contains(name, "AMD") || boost::contains(name, "ATI"))
					{
						_vendorId = 0x1002;
					}
					else if (boost::contains(name, "Intel"))
					{
						_vendorId = 0x8086;
					}
				}
			}
		}

		bool GLRuntime::OnInit(unsigned int width, unsigned int height)
		{
			assert(width != 0 && height != 0);

			_width = width;
			_height = height;
			_window.reset(new WindowWatcher(WindowFromDC(_hdc)));

			// Clear errors
			GLenum status = glGetError();

			_stateBlock->Capture();

			#pragma region Generate backbuffer targets
			GLCHECK(glGenRenderbuffers(2, _defaultBackBufferRBO));

			GLCHECK(glBindRenderbuffer(GL_RENDERBUFFER, _defaultBackBufferRBO[0]));
			glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
			GLCHECK(glBindRenderbuffer(GL_RENDERBUFFER, _defaultBackBufferRBO[1]));
			glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);

			status = glGetError();

			GLCHECK(glBindRenderbuffer(GL_RENDERBUFFER, 0));

			if (status != GL_NO_ERROR)
			{
				LOG(TRACE) << "Failed to create backbuffer renderbuffer with error code " << status;

				GLCHECK(glDeleteRenderbuffers(2, _defaultBackBufferRBO));

				return false;
			}

			GLCHECK(glGenFramebuffers(1, &_defaultBackBufferFBO));

			GLCHECK(glBindFramebuffer(GL_FRAMEBUFFER, _defaultBackBufferFBO));
			GLCHECK(glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, _defaultBackBufferRBO[0]));
			GLCHECK(glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, _defaultBackBufferRBO[1]));

			status = glCheckFramebufferStatus(GL_FRAMEBUFFER);

			GLCHECK(glBindFramebuffer(GL_FRAMEBUFFER, 0));

			if (status != GL_FRAMEBUFFER_COMPLETE)
			{
				LOG(TRACE) << "Failed to create backbuffer framebuffer object with status code " << status;

				GLCHECK(glDeleteFramebuffers(1, &_defaultBackBufferFBO));
				GLCHECK(glDeleteRenderbuffers(2, _defaultBackBufferRBO));

				return false;
			}

			GLCHECK(glGenTextures(2, _backbufferTexture));

			GLCHECK(glBindTexture(GL_TEXTURE_2D, _backbufferTexture[0]));
			glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
			glTextureView(_backbufferTexture[1], GL_TEXTURE_2D, _backbufferTexture[0], GL_SRGB8_ALPHA8, 0, 1, 0, 1);
		
			status = glGetError();

			GLCHECK(glBindTexture(GL_TEXTURE_2D, 0));

			if (status != GL_NO_ERROR)
			{
				LOG(TRACE) << "Failed to create backbuffer texture with error code " << status;

				GLCHECK(glDeleteTextures(2, _backbufferTexture));
				GLCHECK(glDeleteFramebuffers(1, &_defaultBackBufferFBO));
				GLCHECK(glDeleteRenderbuffers(2, _defaultBackBufferRBO));

				return false;
			}
			#pragma endregion

			#pragma region Generate depthbuffer targets
			const DepthSourceInfo defaultdepth = { width, height, 0, GL_DEPTH24_STENCIL8 };

			_depthSourceTable[0] = defaultdepth;

			LOG(TRACE) << "Switched depth source to default depthstencil.";

			GLCHECK(glGenTextures(1, &_depthTexture));

			GLCHECK(glBindTexture(GL_TEXTURE_2D, _depthTexture));
			glTexStorage2D(GL_TEXTURE_2D, 1, defaultdepth.Format, defaultdepth.Width, defaultdepth.Height);

			status = glGetError();

			GLCHECK(glBindTexture(GL_TEXTURE_2D, 0));

			if (status != GL_NO_ERROR)
			{
				LOG(TRACE) << "Failed to create depth texture with error code " << status;

				GLCHECK(glDeleteTextures(1, &_depthTexture));
				GLCHECK(glDeleteTextures(2, _backbufferTexture));
				GLCHECK(glDeleteFramebuffers(1, &_defaultBackBufferFBO));
				GLCHECK(glDeleteRenderbuffers(2, _defaultBackBufferRBO));

				return false;
			}
			#pragma endregion

			GLCHECK(glGenFramebuffers(1, &_blitFBO));

			GLCHECK(glBindFramebuffer(GL_FRAMEBUFFER, _blitFBO));
			GLCHECK(glFramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, _depthTexture, 0));
			GLCHECK(glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, _backbufferTexture[1], 0));

			status = glCheckFramebufferStatus(GL_FRAMEBUFFER);

			GLCHECK(glBindFramebuffer(GL_FRAMEBUFFER, 0));

			if (status != GL_FRAMEBUFFER_COMPLETE)
			{
				LOG(TRACE) << "Failed to create blit framebuffer object with status code " << status;

				GLCHECK(glDeleteFramebuffers(1, &_blitFBO));
				GLCHECK(glDeleteTextures(1, &_depthTexture));
				GLCHECK(glDeleteTextures(2, _backbufferTexture));
				GLCHECK(glDeleteFramebuffers(1, &_defaultBackBufferFBO));
				GLCHECK(glDeleteRenderbuffers(2, _defaultBackBufferRBO));

				return false;
			}


			GLCHECK(glGenVertexArrays(1, &_defaultVAO));
			GLCHECK(glGenBuffers(1, &_defaultVBO));

			GLCHECK(glBindBuffer(GL_ARRAY_BUFFER, _defaultVBO));
			GLCHECK(glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_STATIC_DRAW));
			GLCHECK(glBindBuffer(GL_ARRAY_BUFFER, 0));


			_gui.reset(new GUI(this, nvgCreateGL3(0)));

			_stateBlock->Apply();

			return Runtime::OnInit();
		}
		void GLRuntime::OnReset()
		{
			if (!_isInitialized)
			{
				return;
			}

			Runtime::OnReset();

			// Destroy NanoVG
			NVGcontext *const nvg = _gui->GetContext();

			_gui.reset();

			nvgDeleteGL3(nvg);

			// Destroy resources
			GLCHECK(glDeleteBuffers(1, &_defaultVBO));
			GLCHECK(glDeleteBuffers(1, &_effectUBO));
			GLCHECK(glDeleteVertexArrays(1, &_defaultVAO));
			GLCHECK(glDeleteFramebuffers(1, &_defaultBackBufferFBO));
			GLCHECK(glDeleteFramebuffers(1, &_depthSourceFBO));
			GLCHECK(glDeleteFramebuffers(1, &_blitFBO));
			GLCHECK(glDeleteRenderbuffers(2, _defaultBackBufferRBO));
			GLCHECK(glDeleteTextures(2, _backbufferTexture));
			GLCHECK(glDeleteTextures(1, &_depthTexture));

			_defaultVBO = 0;
			_effectUBO = 0;
			_defaultVAO = 0;
			_defaultBackBufferFBO = 0;
			_depthSourceFBO = 0;
			_blitFBO = 0;
			_defaultBackBufferRBO[0] = 0;
			_defaultBackBufferRBO[1] = 0;
			_backbufferTexture[0] = 0;
			_backbufferTexture[1] = 0;
			_depthTexture = 0;

			_depthSource = 0;
		}
		void GLRuntime::OnResetEffect()
		{
			Runtime::OnResetEffect();

			for (auto &sampler : _effectSamplers)
			{
				GLCHECK(glDeleteSamplers(1, &sampler.ID));
			}

			_effectSamplers.clear();
		}
		void GLRuntime::OnPresent()
		{
			if (!_isInitialized)
			{
				LOG(TRACE) << "Failed to present! Runtime is in a lost state.";
				return;
			}
			else if (_stats.DrawCalls == 0)
			{
				return;
			}

			DetectDepthSource();

			// Capture states
			_stateBlock->Capture();

			// Copy backbuffer
			GLCHECK(glBindFramebuffer(GL_READ_FRAMEBUFFER, 0));
			GLCHECK(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _defaultBackBufferFBO));
			GLCHECK(glReadBuffer(GL_BACK));
			GLCHECK(glDrawBuffer(GL_COLOR_ATTACHMENT0));
			GLCHECK(glBlitFramebuffer(0, 0, _width, _height, 0, 0, _width, _height, GL_COLOR_BUFFER_BIT, GL_NEAREST));

			// Copy depthbuffer
			GLCHECK(glBindFramebuffer(GL_READ_FRAMEBUFFER, _depthSourceFBO));
			GLCHECK(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _blitFBO));
			GLCHECK(glBlitFramebuffer(0, 0, _width, _height, 0, 0, _width, _height, GL_DEPTH_BUFFER_BIT, GL_NEAREST));

			// Apply post processing
			OnApplyEffect();

			glDisable(GL_FRAMEBUFFER_SRGB);

			// Reset rendertarget and copy to backbuffer
			GLCHECK(glBindFramebuffer(GL_READ_FRAMEBUFFER, _defaultBackBufferFBO));
			GLCHECK(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0));
			GLCHECK(glReadBuffer(GL_COLOR_ATTACHMENT0));
			GLCHECK(glDrawBuffer(GL_BACK));
			GLCHECK(glBlitFramebuffer(0, 0, _width, _height, 0, 0, _width, _height, GL_COLOR_BUFFER_BIT, GL_NEAREST));
			GLCHECK(glViewport(0, 0, _width, _height));

			// Apply presenting
			Runtime::OnPresent();

			// Apply states
			_stateBlock->Apply();
		}
		void GLRuntime::OnDrawCall(unsigned int vertices)
		{
			Runtime::OnDrawCall(vertices);

			GLint fbo = 0, object = 0, objecttarget = GL_NONE;
			GLCHECK(glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo));

			if (fbo != 0)
			{
				GLCHECK(glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &objecttarget));

				if (objecttarget == GL_NONE)
				{
					return;
				}
				else
				{
					GLCHECK(glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &object));
				}
			}

			const auto it = _depthSourceTable.find(object | (objecttarget == GL_RENDERBUFFER ? 0x80000000 : 0));

			if (it != _depthSourceTable.end())
			{
				it->second.DrawCallCount = static_cast<GLfloat>(_stats.DrawCalls);
				it->second.DrawVerticesCount += vertices;
			}
		}
		void GLRuntime::OnApplyEffect()
		{
			if (!_isEffectCompiled)
			{
				return;
			}

			// Setup vertex input
			GLCHECK(glBindVertexArray(_defaultVAO));
			GLCHECK(glBindVertexBuffer(0, _defaultVBO, 0, sizeof(float)));

			// Setup shader resources
			for (GLsizei sampler = 0, samplerCount = static_cast<GLsizei>(_effectSamplers.size()); sampler < samplerCount; sampler++)
			{
				GLCHECK(glActiveTexture(GL_TEXTURE0 + sampler));
				GLCHECK(glBindTexture(GL_TEXTURE_2D, _effectSamplers[sampler].Texture->ID[_effectSamplers[sampler].SRGB]));
				GLCHECK(glBindSampler(sampler, _effectSamplers[sampler].ID));
			}

			// Setup shader constants
			GLCHECK(glBindBufferBase(GL_UNIFORM_BUFFER, 0, _effectUBO));

			// Apply post processing
			Runtime::OnApplyEffect();

			// Reset states
			GLCHECK(glBindSampler(0, 0));
		}
		void GLRuntime::OnApplyEffectTechnique(const Technique *technique)
		{
			Runtime::OnApplyEffectTechnique(technique);

			// Clear depthstencil
			GLCHECK(glBindFramebuffer(GL_FRAMEBUFFER, _defaultBackBufferFBO));
			GLCHECK(glClearBufferfi(GL_DEPTH_STENCIL, 0, 1.0f, 0));

			// Update shader constants
			if (_effectUBO != 0)
			{
				GLCHECK(glBufferSubData(GL_UNIFORM_BUFFER, 0, _uniformDataStorage.size(), _uniformDataStorage.data()));
			}

			for (const auto &pass : static_cast<const GLTechnique *>(technique)->Passes)
			{
				// Setup states
				GLCHECK(glUseProgram(pass.Program));
				GLCHECK(glEnableb(GL_FRAMEBUFFER_SRGB, pass.FramebufferSRGB));
				GLCHECK(glDisable(GL_SCISSOR_TEST));
				GLCHECK(glFrontFace(GL_CCW));
				GLCHECK(glPolygonMode(GL_FRONT_AND_BACK, GL_FILL));
				GLCHECK(glDisable(GL_CULL_FACE));
				GLCHECK(glColorMask(pass.ColorMaskR, pass.ColorMaskG, pass.ColorMaskB, pass.ColorMaskA));
				GLCHECK(glEnableb(GL_BLEND, pass.Blend));
				GLCHECK(glBlendFunc(pass.BlendFuncSrc, pass.BlendFuncDest));
				GLCHECK(glBlendEquationSeparate(pass.BlendEqColor, pass.BlendEqAlpha));
				GLCHECK(glEnableb(GL_DEPTH_TEST, pass.DepthTest));
				GLCHECK(glDepthMask(pass.DepthMask));
				GLCHECK(glDepthFunc(pass.DepthFunc));
				GLCHECK(glEnableb(GL_STENCIL_TEST, pass.StencilTest));
				GLCHECK(glStencilFunc(pass.StencilFunc, pass.StencilRef, pass.StencilReadMask));
				GLCHECK(glStencilOp(pass.StencilOpFail, pass.StencilOpZFail, pass.StencilOpZPass));
				GLCHECK(glStencilMask(pass.StencilMask));

				// Save backbuffer of previous pass
				GLCHECK(glBindFramebuffer(GL_READ_FRAMEBUFFER, _defaultBackBufferFBO));
				GLCHECK(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _blitFBO));
				GLCHECK(glReadBuffer(GL_COLOR_ATTACHMENT0));
				GLCHECK(glDrawBuffer(GL_COLOR_ATTACHMENT0));
				GLCHECK(glBlitFramebuffer(0, 0, _width, _height, 0, 0, _width, _height, GL_COLOR_BUFFER_BIT, GL_NEAREST));

				// Setup rendertargets
				GLCHECK(glBindFramebuffer(GL_FRAMEBUFFER, pass.Framebuffer));
				GLCHECK(glDrawBuffers(8, pass.DrawBuffers));
				GLCHECK(glViewport(0, 0, pass.ViewportWidth, pass.ViewportHeight));

				for (GLuint k = 0; k < 8; k++)
				{
					if (pass.DrawBuffers[k] == GL_NONE)
					{
						continue;
					}

					const GLfloat color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
					GLCHECK(glClearBufferfv(GL_COLOR, k, color));
				}

				// Draw triangle
				GLCHECK(glDrawArrays(GL_TRIANGLES, 0, 3));

				// Update shader resources
				for (GLuint id : pass.DrawTextures)
				{
					for (GLsizei sampler = 0, samplerCount = static_cast<GLsizei>(_effectSamplers.size()); sampler < samplerCount; sampler++)
					{
						const GLTexture *const texture = _effectSamplers[sampler].Texture;

						if (texture->Levels > 1 && (texture->ID[0] == id || texture->ID[1] == id))
						{
							GLCHECK(glActiveTexture(GL_TEXTURE0 + sampler));
							GLCHECK(glGenerateMipmap(GL_TEXTURE_2D));
						}
					}
				}
			}
		}

		void GLRuntime::OnFramebufferAttachment(GLenum target, GLenum attachment, GLenum objecttarget, GLuint object, GLint level)
		{
			if (object == 0 || (attachment != GL_DEPTH_ATTACHMENT && attachment != GL_DEPTH_STENCIL_ATTACHMENT))
			{
				return;
			}

			// Get current framebuffer
			GLint fbo = 0;
			GLCHECK(glGetIntegerv(TargetToBinding(target), &fbo));

			assert(fbo != 0);

			if (static_cast<GLuint>(fbo) == _defaultBackBufferFBO || static_cast<GLuint>(fbo) == _depthSourceFBO || static_cast<GLuint>(fbo) == _blitFBO)
			{
				return;
			}

			const GLuint id = object | (objecttarget == GL_RENDERBUFFER ? 0x80000000 : 0);
		
			if (_depthSourceTable.find(id) != _depthSourceTable.end())
			{
				return;
			}

			DepthSourceInfo info = { 0, 0, 0, GL_NONE };

			if (objecttarget == GL_RENDERBUFFER)
			{
				GLint previous = 0;
				GLCHECK(glGetIntegerv(GL_RENDERBUFFER_BINDING, &previous));

				// Get depthstencil parameters from renderbuffer
				GLCHECK(glBindRenderbuffer(GL_RENDERBUFFER, object));
				GLCHECK(glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &info.Width));
				GLCHECK(glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &info.Height));
				GLCHECK(glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_INTERNAL_FORMAT, &info.Format));

				GLCHECK(glBindRenderbuffer(GL_RENDERBUFFER, previous));
			}
			else
			{
				if (objecttarget == GL_TEXTURE)
				{
					objecttarget = GL_TEXTURE_2D;
				}

				GLint previous = 0;
				GLCHECK(glGetIntegerv(TargetToBinding(objecttarget), &previous));

				// Get depthstencil parameters from texture
				GLCHECK(glBindTexture(objecttarget, object));
				info.Level = level;
				GLCHECK(glGetTexLevelParameteriv(objecttarget, level, GL_TEXTURE_WIDTH, &info.Width));
				GLCHECK(glGetTexLevelParameteriv(objecttarget, level, GL_TEXTURE_HEIGHT, &info.Height));
				GLCHECK(glGetTexLevelParameteriv(objecttarget, level, GL_TEXTURE_INTERNAL_FORMAT, &info.Format));
			
				GLCHECK(glBindTexture(objecttarget, previous));
			}

			LOG(TRACE) << "Adding framebuffer " << fbo << " attachment " << object << " (Attachment Type: " << attachment << ", Object Type: " << objecttarget << ", Width: " << info.Width << ", Height: " << info.Height << ", Format: " << info.Format << ") to list of possible depth candidates ...";

			_depthSourceTable.emplace(id, info);
		}

		void GLRuntime::Screenshot(unsigned char *buffer) const
		{
			GLCHECK(glReadBuffer(GL_BACK));
			GLCHECK(glReadPixels(0, 0, static_cast<GLsizei>(_width), static_cast<GLsizei>(_height), GL_RGBA, GL_UNSIGNED_BYTE, buffer));

			// Flip image
			const unsigned int pitch = _width * 4;

			for (unsigned int y = 0; y * 2 < _height; ++y)
			{
				const unsigned int i1 = y * pitch;
				const unsigned int i2 = (_height - 1 - y) * pitch;

				for (unsigned int x = 0; x < pitch; x += 4)
				{
					buffer[i1 + x + 3] = 0xFF;
					buffer[i2 + x + 3] = 0xFF;

					std::swap(buffer[i1 + x + 0], buffer[i2 + x + 0]);
					std::swap(buffer[i1 + x + 1], buffer[i2 + x + 1]);
					std::swap(buffer[i1 + x + 2], buffer[i2 + x + 2]);
				}
			}
		}
		bool GLRuntime::UpdateEffect(const FX::NodeTree &ast, const std::vector<std::string> &/*pragmas*/, std::string &errors)
		{
			GLEffectCompiler visitor(ast);

			return visitor.Compile(this, errors);
		}
		bool GLRuntime::UpdateTexture(Texture *texture, const unsigned char *data, size_t size)
		{
			GLTexture *const textureImpl = dynamic_cast<GLTexture *>(texture);

			assert(textureImpl != nullptr);
			assert(data != nullptr && size > 0);

			if (textureImpl->DataSource != GLTexture::Source::Memory)
			{
				return false;
			}

			GLCHECK(glPixelStorei(GL_UNPACK_ROW_LENGTH, 0));
			GLCHECK(glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0));
			GLCHECK(glPixelStorei(GL_UNPACK_SKIP_ROWS, 0));
			GLCHECK(glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0));

			GLint previous = 0;
			GLCHECK(glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous));

			// Copy image data
			const std::unique_ptr<unsigned char[]> dataFlipped(new unsigned char[size]);
			std::memcpy(dataFlipped.get(), data, size);

			// Flip image data vertically
			FlipImageData(texture, dataFlipped.get());

			// Bind and update texture
			GLCHECK(glBindTexture(GL_TEXTURE_2D, textureImpl->ID[0]));

			if (texture->Format >= Texture::PixelFormat::DXT1 && texture->Format <= Texture::PixelFormat::LATC2)
			{
				GLCHECK(glCompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, texture->Width, texture->Height, GL_UNSIGNED_BYTE, static_cast<GLsizei>(size), dataFlipped.get()));
			}
			else
			{
				GLint dataAlignment = 4;
				GLenum dataFormat = GL_RGBA, dataType = GL_UNSIGNED_BYTE;

				switch (texture->Format)
				{
					case Texture::PixelFormat::R8:
						dataFormat = GL_RED;
						dataAlignment = 1;
						break;
					case Texture::PixelFormat::R16F:
						dataType = GL_UNSIGNED_SHORT;
						dataFormat = GL_RED;
						dataAlignment = 2;
						break;
					case Texture::PixelFormat::R32F:
						dataType = GL_FLOAT;
						dataFormat = GL_RED;
						break;
					case Texture::PixelFormat::RG8:
						dataFormat = GL_RG;
						dataAlignment = 2;
						break;
					case Texture::PixelFormat::RG16:
					case Texture::PixelFormat::RG16F:
						dataType = GL_UNSIGNED_SHORT;
						dataFormat = GL_RG;
						dataAlignment = 2;
						break;
					case Texture::PixelFormat::RG32F:
						dataType = GL_FLOAT;
						dataFormat = GL_RG;
						break;
					case Texture::PixelFormat::RGBA16:
					case Texture::PixelFormat::RGBA16F:
						dataType = GL_UNSIGNED_SHORT;
						dataAlignment = 2;
						break;
					case Texture::PixelFormat::RGBA32F:
						dataType = GL_FLOAT;
						break;
				}

				GLCHECK(glPixelStorei(GL_UNPACK_ALIGNMENT, dataAlignment));
				GLCHECK(glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, texture->Width, texture->Height, dataFormat, dataType, dataFlipped.get()));
				GLCHECK(glPixelStorei(GL_UNPACK_ALIGNMENT, 4));
			}

			if (texture->Levels > 1)
			{
				GLCHECK(glGenerateMipmap(GL_TEXTURE_2D));
			}

			GLCHECK(glBindTexture(GL_TEXTURE_2D, previous));

			return true;
		}

		void GLRuntime::DetectDepthSource()
		{
			static int cooldown = 0, traffic = 0;

			if (cooldown-- > 0)
			{
				traffic += NetworkUpload > 0;
				return;
			}
			else
			{
				cooldown = 30;

				if (traffic > 10)
				{
					traffic = 0;
					_depthSource = 0;
					CreateDepthTexture(0, 0, GL_NONE);
					return;
				}
				else
				{
					traffic = 0;
				}
			}

			GLuint best = 0;
			DepthSourceInfo bestInfo = { 0, 0, 0, GL_NONE };

			for (auto &it : _depthSourceTable)
			{
				if (it.second.DrawCallCount == 0)
				{
					continue;
				}
				else if (((it.second.DrawVerticesCount * (1.2f - it.second.DrawCallCount / _stats.DrawCalls)) >= (bestInfo.DrawVerticesCount * (1.2f - bestInfo.DrawCallCount / _stats.DrawCalls))) && ((it.second.Width > _width * 0.95 && it.second.Width < _width * 1.05) && (it.second.Height > _height * 0.95 && it.second.Height < _height * 1.05)))
				{
					best = it.first;
					bestInfo = it.second;
				}

				it.second.DrawCallCount = it.second.DrawVerticesCount = 0;
			}

			if (best == 0)
			{
				bestInfo = _depthSourceTable.at(0);
			}

			if (_depthSource != best || _depthTexture == 0)
			{
				const DepthSourceInfo &previousInfo = _depthSourceTable.at(_depthSource);

				if ((bestInfo.Width != previousInfo.Width || bestInfo.Height != previousInfo.Height || bestInfo.Format != previousInfo.Format) || _depthTexture == 0)
				{
					// Resize depth texture
					CreateDepthTexture(bestInfo.Width, bestInfo.Height, bestInfo.Format);
				}

				GLint previousFBO = 0;
				GLCHECK(glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFBO));

				GLCHECK(glBindFramebuffer(GL_FRAMEBUFFER, _blitFBO));
				GLCHECK(glFramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, _depthTexture, 0));

				assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

				_depthSource = best;

				if (best != 0)
				{
					if (_depthSourceFBO == 0)
					{
						GLCHECK(glGenFramebuffers(1, &_depthSourceFBO));
					}

					GLCHECK(glBindFramebuffer(GL_FRAMEBUFFER, _depthSourceFBO));

					if ((best & 0x80000000) != 0)
					{
						best ^= 0x80000000;

						LOG(TRACE) << "Switched depth source to renderbuffer " << best << ".";

						GLCHECK(glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, best));
					}
					else
					{
						LOG(TRACE) << "Switched depth source to texture " << best << ".";

						GLCHECK(glFramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, best, bestInfo.Level));
					}

					const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);

					if (status != GL_FRAMEBUFFER_COMPLETE)
					{
						LOG(TRACE) << "Failed to create depth source framebuffer with status code " << status << ".";

						GLCHECK(glDeleteFramebuffers(1, &_depthSourceFBO));
						_depthSourceFBO = 0;
					}
				}
				else
				{
					LOG(TRACE) << "Switched depth source to default framebuffer.";

					GLCHECK(glDeleteFramebuffers(1, &_depthSourceFBO));
					_depthSourceFBO = 0;
				}

				GLCHECK(glBindFramebuffer(GL_FRAMEBUFFER, previousFBO));
			}
		}
		void GLRuntime::CreateDepthTexture(GLuint width, GLuint height, GLenum format)
		{
			GLCHECK(glDeleteTextures(1, &_depthTexture));

			if (format != GL_NONE)
			{
				GLint previousTex = 0;
				GLCHECK(glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTex));

				// Clear errors
				GLenum status = glGetError();

				GLCHECK(glGenTextures(1, &_depthTexture));

				GLCHECK(glBindTexture(GL_TEXTURE_2D, _depthTexture));
				glTexStorage2D(GL_TEXTURE_2D, 1, format, width, height);

				status = glGetError();

				if (status != GL_NO_ERROR)
				{
					LOG(ERROR) << "Failed to create depth texture for format " << format << " with error code " << status;

					GLCHECK(glDeleteTextures(1, &_depthTexture));

					_depthTexture = 0;
				}

				GLCHECK(glBindTexture(GL_TEXTURE_2D, previousTex));
			}
			else
			{
				_depthTexture = 0;
			}

			// Update effect textures
			for (const auto &it : _textures)
			{
				GLTexture *texture = static_cast<GLTexture *>(it.get());

				if (texture->DataSource == GLTexture::Source::DepthStencil)
				{
					texture->ChangeDataSource(GLTexture::Source::DepthStencil, _depthTexture, 0);
				}
			}
		}
	}
}
