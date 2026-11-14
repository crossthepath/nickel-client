#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <wrl/client.h>
#include <fstream>
#include <iterator>
#include <vector>
#include <cstdio>
using Microsoft::WRL::ComPtr;
int main(int argc,char** argv){
 if(argc!=3)return 2;
 std::ifstream input(argv[1],std::ios::binary);if(!input)return 3;
 std::vector<char> data((std::istreambuf_iterator<char>(input)),{});if(data.empty())return 4;
 ComPtr<ID3D11ShaderReflection> reflection;HRESULT result=D3DReflect(data.data(),data.size(),IID_ID3D11ShaderReflection,(void**)reflection.GetAddressOf());
 if(SUCCEEDED(result)){
  D3D11_SHADER_DESC desc{};if(FAILED(reflection->GetDesc(&desc)))return 5;
  std::printf("type=%u version=%u instructions=%u resources=%u\n",D3D11_SHVER_GET_TYPE(desc.Version),desc.Version,desc.InstructionCount,desc.BoundResources);
  for(UINT i=0;i<desc.BoundResources;++i){D3D11_SHADER_INPUT_BIND_DESC binding{};reflection->GetResourceBindingDesc(i,&binding);std::printf("resource %s type=%u slot=%u count=%u\n",binding.Name,binding.Type,binding.BindPoint,binding.BindCount);}
  for(UINT i=0;i<desc.ConstantBuffers;++i){auto* buffer=reflection->GetConstantBufferByIndex(i);D3D11_SHADER_BUFFER_DESC cb{};buffer->GetDesc(&cb);std::printf("buffer %s bytes=%u\n",cb.Name,cb.Size);for(UINT j=0;j<cb.Variables;++j){D3D11_SHADER_VARIABLE_DESC var{};buffer->GetVariableByIndex(j)->GetDesc(&var);std::printf(" variable %s offset=%u bytes=%u\n",var.Name,var.StartOffset,var.Size);}}
 }else std::printf("reflection unavailable HRESULT=%08lx\n",(unsigned long)result);
 ComPtr<ID3DBlob> assembly;result=D3DDisassemble(data.data(),data.size(),0,nullptr,&assembly);
 if(FAILED(result)){std::printf("disassembly unavailable HRESULT=%08lx\n",(unsigned long)result);return 6;}
 std::ofstream output(argv[2],std::ios::binary);if(!output)return 7;output.write((const char*)assembly->GetBufferPointer(),assembly->GetBufferSize());return output?0:8;
}
