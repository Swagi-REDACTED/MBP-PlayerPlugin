#include <cassert>
#include <cmath>
#include <string>
#include "moviebox_protocol.hpp"

int main() {
    mbp::PlaybackRequest request;
    request.action = "episode";
    request.revision = 42;
    request.position = 12.5;
    request.duration = 100.0;
    request.playing = true;
    request.ready = true;
    request.volume = 73;
    request.muted = false;
    request.metadata = true;
    request.id = "2:7\"\\x";
    request.value = -0.5;

    const std::string encoded = mbp::SerializePlaybackRequest(request);
    assert(encoded.find("\"Action\":\"episode\"") != std::string::npos);
    assert(encoded.find("\"Revision\":42") != std::string::npos);
    assert(encoded.find("\"Id\":\"2:7\\\"\\\\x\"") != std::string::npos);
    assert(encoded.find("\"Metadata\":true") != std::string::npos);

    const std::string json = R"JSON({
      "Error":null,
      "Source":{"Revision":7,"Uri":"https://example/a\\b.mp4?q=\"x\"","Title":"Show \u263A","Engine":"FFmpeg","Seconds":91.25,"Playing":false,"Rate":1.25,"SettingsTitle":"Show","Season":2,"Episode":7,"BoxType":2},
      "Metadata":{"IsSeries":true,"Loading":false,"Error":null,
        "Episodes":[{"Id":"2:7","Label":"E7  Finale","Group":"Season 2","Selected":true}],
        "Qualities":[{"Id":"q1","Label":"1080p","Group":"","Selected":true}],
        "Subtitles":[{"Id":"s1","Label":"English","Group":"English","Selected":false}],
        "Servers":[{"Id":"srv","Label":"US","Group":"","Selected":true}]},
      "Commands":[{"Action":"Pause","Values":{"Pause":true}},{"Action":"Time","Values":{"Percent":50.0}}],
      "Cues":[{"Start":1.5,"End":4.0,"Text":"Hello \nworld"}],
      "SubtitleDelay":-0.5,
      "Closed":false
    })JSON";

    mbp::PlaybackReply reply;
    std::string error;
    assert(mbp::ParsePlaybackReply(json, reply, error));
    assert(error.empty());
    assert(reply.source.has_value());
    assert(reply.source->revision == 7);
    assert(reply.source->uri == "https://example/a\\b.mp4?q=\"x\"");
    assert(reply.source->title.find("Show ") == 0);
    assert(reply.source->season == 2 && reply.source->episode == 7 && reply.source->boxType == 2);
    assert(std::fabs(reply.source->seconds - 91.25) < 1e-9);
    assert(std::fabs(reply.source->rate - 1.25) < 1e-9);
    assert(!reply.source->playing);

    assert(reply.metadata.has_value());
    assert(reply.metadata->isSeries);
    assert(reply.metadata->episodes.size() == 1);
    assert(reply.metadata->episodes[0].group == "Season 2");
    assert(reply.metadata->qualities[0].label == "1080p");
    assert(reply.metadata->servers[0].selected);

    assert(reply.commands.size() == 2);
    assert(reply.commands[0].action == "Pause");
    assert(reply.commands[0].values.at("Pause").asBool(false));
    assert(std::fabs(reply.commands[1].values.at("Percent").asNumber(0) - 50.0) < 1e-9);

    assert(reply.cues.has_value());
    assert(reply.cues->size() == 1);
    assert((*reply.cues)[0].text == "Hello \nworld");
    assert(std::fabs(reply.subtitleDelay + 0.5) < 1e-9);
    assert(!reply.closed);

    mbp::PlaybackReply nulls;
    assert(mbp::ParsePlaybackReply(R"({"Source":null,"Metadata":null,"Commands":[],"Cues":null,"SubtitleDelay":0,"Closed":true})", nulls, error));
    assert(!nulls.source.has_value());
    assert(!nulls.metadata.has_value());
    assert(!nulls.cues.has_value());
    assert(nulls.closed);

    mbp::PlaybackReply bad;
    assert(!mbp::ParsePlaybackReply("{bad", bad, error));
    assert(!error.empty());
    return 0;
}
