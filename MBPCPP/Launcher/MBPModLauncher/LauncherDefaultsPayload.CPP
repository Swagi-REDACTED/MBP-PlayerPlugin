#include "LauncherDefaultsPayload.HPP"
#include <algorithm>
#include <cstddef>
#include <limits>
namespace launcher {
namespace {
constexpr std::uint8_t kVersion=1, kSubtitleOff=1u<<0, kQualityAuto=1u<<1, kServerAuto=1u<<2, kKnownFlags=7;
void failmsg(std::string* e,const char* m){if(e)*e=m;}
void w16(std::string& o,std::uint16_t v){o.push_back(char(v&255));o.push_back(char((v>>8)&255));}
std::uint16_t r16(std::string_view b,std::size_t p){return std::uint16_t((unsigned char)b[p])|(std::uint16_t((unsigned char)b[p+1])<<8);}
std::string trim(std::string s){auto isws=[](unsigned char c){return c==' '||c=='\t'||c=='\r'||c=='\n';}; while(!s.empty()&&isws((unsigned char)s.front()))s.erase(s.begin()); while(!s.empty()&&isws((unsigned char)s.back()))s.pop_back(); return s;}
}
bool EncodeLdef(const LauncherSettingsData& in,std::string& out,std::string* error){
 if(error)error->clear();
 if(in.movieBoxPathUtf8.size()>65535){failmsg(error,"MovieBox path exceeds 65535 UTF-8 bytes.");return false;}
 std::string lang=in.playerDefaults.subtitleLanguage, server=in.playerDefaults.serverPreference;
 std::uint8_t flags=0; std::uint16_t q=in.playerDefaults.targetQualityHeight;
 if(in.playerDefaults.subtitleMode==SubtitleDefaultMode::Off){flags|=kSubtitleOff;lang.clear();} else if(lang.empty()){failmsg(error,"Subtitle language is required.");return false;}
 if(in.playerDefaults.qualityMode==QualityDefaultMode::Auto){flags|=kQualityAuto;q=0;} else if(q==0){failmsg(error,"Target quality height is required.");return false;}
 if(in.playerDefaults.serverMode==ServerDefaultMode::Auto){flags|=kServerAuto;server.clear();} else {server=trim(server); if(server.empty()){failmsg(error,"Preferred server is required.");return false;}}
 if(lang.size()>255||server.size()>255){failmsg(error,"Language/server exceeds 255 UTF-8 bytes.");return false;}
 const std::uint8_t volume=std::min<std::uint8_t>(100,in.playerDefaults.volume);
 out.clear(); out.reserve(13+in.movieBoxPathUtf8.size()+lang.size()+server.size()); out.append("LDEF",4); out.push_back(char(kVersion)); out.push_back(char(flags)); out.push_back(char(volume)); w16(out,q); w16(out,(std::uint16_t)in.movieBoxPathUtf8.size()); out.push_back(char(lang.size())); out.push_back(char(server.size())); out+=in.movieBoxPathUtf8; out+=lang; out+=server; return true;
}
bool DecodeLdef(std::string_view b,LauncherSettingsData& out,std::string* error){
 if(error) error->clear();
 if(b.size()<13){failmsg(error,"LDEF header is truncated.");return false;}
 if(b.substr(0,4)!="LDEF"){failmsg(error,"Invalid LDEF magic.");return false;}
 if((unsigned char)b[4]!=kVersion){failmsg(error,"Unsupported LDEF version.");return false;}
 const auto flags=(std::uint8_t)b[5];
 if(flags&~kKnownFlags){failmsg(error,"Unknown LDEF flag bits.");return false;}
 const std::uint8_t vol=(std::uint8_t)b[6];
 const std::uint16_t q=r16(b,7), pn=r16(b,9);
 const std::uint8_t ln=(std::uint8_t)b[11], sn=(std::uint8_t)b[12];
 const std::size_t need=13u+pn+ln+sn;
 if(need!=b.size()){failmsg(error,"LDEF lengths do not match payload.");return false;}
 LauncherSettingsData x{}; std::size_t p=13; x.movieBoxPathUtf8.assign(b.substr(p,pn)); p+=pn; x.playerDefaults.subtitleLanguage.assign(b.substr(p,ln)); p+=ln; x.playerDefaults.serverPreference.assign(b.substr(p,sn)); x.playerDefaults.volume=std::min<std::uint8_t>(100,vol);
 if(flags&kSubtitleOff){if(ln!=0){failmsg(error,"Subtitle Off must have empty language.");return false;} x.playerDefaults.subtitleMode=SubtitleDefaultMode::Off;} else {if(ln==0){failmsg(error,"Language mode requires language.");return false;} x.playerDefaults.subtitleMode=SubtitleDefaultMode::Language;}
 if(flags&kQualityAuto){if(q!=0){failmsg(error,"Quality Auto requires zero target.");return false;} x.playerDefaults.qualityMode=QualityDefaultMode::Auto;} else {if(q==0){failmsg(error,"Target quality requires nonzero target.");return false;} x.playerDefaults.qualityMode=QualityDefaultMode::TargetHeight; x.playerDefaults.targetQualityHeight=q;}
 if(flags&kServerAuto){if(sn!=0){failmsg(error,"Server Auto must have empty preference.");return false;} x.playerDefaults.serverMode=ServerDefaultMode::Auto;} else {if(sn==0){failmsg(error,"Preferred server requires value.");return false;} x.playerDefaults.serverMode=ServerDefaultMode::Preferred;}
 out=std::move(x); return true;
}
}
