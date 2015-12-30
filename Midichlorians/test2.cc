/*******************************************************************************
The MIT License (MIT)

Copyright (c) 2015 Maxim Zhurovich <zhurovich@gmail.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*******************************************************************************/

// Use the define below to enable debug output via `NSLog`.
// #define CURRENT_APPLE_ENABLE_NSLOG

#include "../port.h"

#ifdef CURRENT_APPLE

// The following line is needed to avoid OS X headers conflicts with C++.
#define __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES 0

#define CURRENT_MOCK_TIME  // `SetNow()`.

#include "Dev/Beta2/iOS/Midichlorians.mm"
#include "Dev/Beta2/iOS/MidichloriansImpl.mm"

#include "../Blocks/HTTP/api.h"

#include "../Bricks/strings/join.h"
#include "../Bricks/template/rtti_dynamic_call.h"

#include "../Bricks/dflags/dflags.h"
#include "../3rdparty/gtest/gtest-main-with-dflags.h"

DEFINE_int32(http_port, 8383, "Port to spawn server on.");
DEFINE_string(http_route, "/log", "HTTP route of the server.");

using namespace current::midichlorians::ios;

struct Server {
  using T_EVENT_VARIANT = Variant<T_IOS_EVENTS>;

  Server(int http_port, const std::string& http_route)
      : messages_processed(0u),
        endpoints(HTTP(http_port).Register(http_route,
                                           [this](Request r) {
                                             T_EVENT_VARIANT event;
                                             try {
                                               event = ParseJSON<T_EVENT_VARIANT>(r.body);
                                               event.Call(*this);
                                               ++messages_processed;
                                             } catch (const current::Exception&) {
                                             }
                                           })) {}

  void operator()(const iOSAppLaunchEvent& event) {
    EXPECT_FALSE(event.device_id.empty());
    EXPECT_FALSE(event.binary_version.empty());
    EXPECT_GT(event.app_install_time, 1420000000000u);
    EXPECT_GT(event.app_update_time, 1420000000000u);
    messages.push_back(ToString(event.user_ms.count()) + ":Launch");
  }

  void operator()(const iOSIdentifyEvent& event) {
    messages.push_back(ToString(event.user_ms.count()) + ":Identify[" + event.client_id + ']');
  }

  void operator()(const iOSFocusEvent& event) {
    if (event.gained_focus) {
      messages.push_back(ToString(event.user_ms.count()) + ":GainedFocus[" + event.source + ']');
    } else {
      messages.push_back(ToString(event.user_ms.count()) + ":LostFocus[" + event.source + ']');
    }
  }

  void operator()(const iOSGenericEvent& event) {
    std::string params = "source=" + event.source;
    for (const auto& f : event.fields) {
      params += ',';
      params += f.first + '=' + f.second;
    }
    messages.push_back(ToString(event.user_ms.count()) + ':' + event.event + '[' + params + ']');
  }

  void operator()(const iOSBaseEvent&) {}

  std::vector<std::string> messages;
  std::atomic_size_t messages_processed;

 private:
  HTTPRoutesScope endpoints;
};

TEST(Midichlorians, SmokeTest) {
  Server server(FLAGS_http_port, FLAGS_http_route);

  current::time::SetNow(std::chrono::microseconds(0));
  NSDictionary* launchOptions = [NSDictionary new];
  [Midichlorians setup:[NSString
                           stringWithFormat:@"http://localhost:%d%s", FLAGS_http_port, FLAGS_http_route.c_str()]
      withLaunchOptions:launchOptions];

  current::time::SetNow(std::chrono::microseconds(1000));
  [Midichlorians focusEvent:YES source:@"applicationDidBecomeActive"];

  current::time::SetNow(std::chrono::microseconds(2000));
  [Midichlorians identify:@"unit_test"];

  current::time::SetNow(std::chrono::microseconds(5000));
  NSDictionary* eventParams = @{ @"s" : @"str", @"b" : @true, @"x" : @1 };
  [Midichlorians trackEvent:@"CustomEvent1" source:@"SmokeTest" properties:eventParams];

  current::time::SetNow(std::chrono::microseconds(15000));
  [Midichlorians focusEvent:NO source:@"applicationDidEnterBackground"];

  while (server.messages_processed < 5u) {
    ;  // spin lock.
  }

  EXPECT_EQ(
      "0:Launch,1:GainedFocus[applicationDidBecomeActive],2:Identify[unit_test],5:CustomEvent1[source="
      "SmokeTest,b=1,s=str,x=1],15:LostFocus[applicationDidEnterBackground]",
      current::strings::Join(server.messages, ','));
}

#endif  // CURRENT_APPLE
