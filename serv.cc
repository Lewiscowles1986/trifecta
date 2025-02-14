#define CPPHTTPLIB_USE_POLL
#include "httplib.h"
#include "sqlwriter.hh"
#include "nlohmann/json.hpp"
#include "bcrypt.h"
#include <iostream>
#include <mutex>
#include "jsonhelper.hh"
#include "support.hh"
#include <fmt/core.h>
#include <stdexcept>
#include "argparse/argparse.hpp" 
#include <random>
using namespace std;

/*
Todo:
  Configuration items in database
  Enable password reset email

  implement expiry in UI
     public=0 really means that
     public=1 means "public until pubicUntil if non-zero"

  implement change my password in UI
  if a user is disabled, do images/posts turn into 404s?

  opengraph data for previews, how? iframe?

  how do we deal with errors?  500? or a JSON thing?
    authentication/authorization error?
    impossible requests?
      "trying to delete an image that does not exist"
*/

std::string& testrunnerPw()
{
  static string testrunnerpw; // this is so the testrunner can get the newly created password
  return testrunnerpw;
}

// helper that makes sure only 1 thread uses the sqlitewriter at a time, plus some glue to emit answers as json
struct LockedSqw
{
  LockedSqw(const LockedSqw&) = delete;
  
  SQLiteWriter& sqw;
  std::mutex& sqwlock;
  vector<unordered_map<string, MiniSQLite::outvar_t>> query(const std::string& query, const std::initializer_list<SQLiteWriter::var_t>& values ={})
  {
    std::lock_guard<mutex> l(sqwlock);
    return sqw.queryT(query, values);
  }

  void queryJ(httplib::Response &res, const std::string& q, const std::initializer_list<SQLiteWriter::var_t>& values) 
  {
    auto result = query(q, values);
    res.set_content(packResultsJsonStr(result), "application/json");
  }

  void addValue(const std::initializer_list<std::pair<const char*, SQLiteWriter::var_t>>& values, const std::string& table="data")
  {
    std::lock_guard<mutex> l(sqwlock);
    sqw.addValue(values, table);
  }
};

static int64_t getRandom63()
{
  static std::random_device rd;
  static std::mt19937_64 generator(rd());
  std::uniform_int_distribution<int64_t> dist(1, std::numeric_limits<int64_t>::max());
  return dist(generator);
}

struct Users
{
  Users(LockedSqw& lsqw) : d_lsqw(lsqw)
  {}
  bool checkPassword(const std::string& user, const std::string& password) const;
  void createUser(const std::string& user, const std::string& password, const std::string& email, bool admin);
  void changePassword(const std::string& user, const std::string& password);
  void delUser(const std::string& user);
  bool userHasCap(const std::string& user, const std::string& cap)
  {
    bool ret=false;
    if(cap=="valid-user") {
      auto c = d_lsqw.query("select count(1) as c from users where user=? and disabled=0", {user});
      ret = (c.size()==1 && get<int64_t>(c[0]["c"])==1);
    }
    else if(cap=="admin") {
      auto c = d_lsqw.query("select count(1) as c from users where user=? and disabled=0 and admin=1", {user});
      ret = (c.size()==1 && get<int64_t>(c[0]["c"])==1);
    }
    return ret;
  }
  LockedSqw& d_lsqw;
};

bool Users::checkPassword(const std::string& user, const std::string& password) const
{
  auto res = d_lsqw.query("select pwhash, caps from users where user=? and disabled=0", {user});
  if(res.empty()) {
    cout<<"No such user '"<< user << "'" <<endl;
    return false;
  }
  //  cout<<"Password: '"<<password<<"'\n";
  return bcrypt::validatePassword(password, get<string>(res[0]["pwhash"]));
}

void Users::createUser(const std::string& user, const std::string& password, const std::string& email, bool admin)
{
  string pwhash = bcrypt::generateHash(password);
  d_lsqw.addValue({{"user", user}, {"pwhash", pwhash}, {"admin", (int)admin}, {"disabled", 0}, {"caps", ""}, {"lastLoginTstamp", 0}, {"email", email}}, "users");
  d_lsqw.addValue({{"action", "create-user"}, {"user", user}, {"ip", "xx missing xx"}, {"tstamp", time(0)}}, "log");
}

void Users::delUser(const std::string& user) 
{
  d_lsqw.query("delete from users where user=?", {user});
}

void Users::changePassword(const std::string& user, const std::string& password)
{
  string pwhash = bcrypt::generateHash(password);
  auto res = d_lsqw.query("select user from users where user=?", {user});
  if(res.size()!=1 || get<string>(res[0]["user"]) != user) {
    d_lsqw.addValue({{"action", "change-password-failure"}, {"user", user}, {"ip", "xx missing xx"}, {"meta", "no such user"}, {"tstamp", time(0)}}, "log");
    throw std::runtime_error("Tried to change password for user '"+user+"', but does not exist");
  }
  d_lsqw.query("update users set pwhash=? where user=?", {pwhash, user});
  d_lsqw.addValue({{"action", "change-password"}, {"user", user}, {"ip", "xx missing xx"}, {"tstamp", time(0)}}, "log");
}

class Sessions
{
public:
  Sessions(LockedSqw& lsqw) : d_lsqw(lsqw)
  {}

  string getUserForSession(const std::string& sessionid, const std::string& agent, const std::string& ip)
  {
    try {
      auto ret = d_lsqw.query("select * from sessions where id=?", {sessionid});
      if(ret.size()==1) {
        d_lsqw.query("update sessions set lastUseTstamp=?, agent=?, ip=? where id=?", {time(0), agent, ip, sessionid});
        return get<string>(ret[0]["user"]);
      }
    }
    catch(...){}
    return "";
  }
  
  string createSessionForUser(const std::string& user, const std::string& agent, const std::string& ip)
  {
    string sessionid=makeShortID(getRandom63())+makeShortID(getRandom63());
    d_lsqw.addValue({{"id", sessionid}, {"user", user}, {"agent", agent}, {"ip", ip}, {"createTstamp", time(0)}, {"lastUseTstamp", 0}}, "sessions");
    return sessionid;
  }

  void dropSession(const std::string& sessionid)
  {
    d_lsqw.query("delete from sessions where id=?", {sessionid});
  }
private:
  LockedSqw& d_lsqw;
};

struct AuthReqs
{
  AuthReqs(Sessions& sessions, Users& users) : d_sessions(sessions), d_users(users)
  {}

  set<string> auths;

  string getSessionID(const httplib::Request &req) const
  {
    auto cookies = getCookies(req.get_header_value("Cookie"));
    auto siter = cookies.find("session");
    if(siter == cookies.end()) {
      throw std::runtime_error("No session cookie");
    }
    return siter->second;
  }
  bool check(const httplib::Request &req) const
  try
  {
    string user = getUser(req);
    for(const auto& a : auths) {
      if(!d_users.userHasCap(user, a)) {
        cout<<"User '"<<user<<"' lacked capability '"<<a<<"'"<<endl;
        return false;
      }
    }
    return true;
  }
  catch(std::exception& e) {
    cout<<"Could not check user capabilities: "<<e.what()<<endl;
    return false;
  }

  void dropSession(const httplib::Request &req) 
  {
    d_sessions.dropSession(getSessionID(req));
  }

  // XXXX should only trust X-Real-IP if traffic is from a known and trusted proxy
  string getIP(const httplib::Request& req) const
  {
    if(req.has_header("X-Real-IP"))
      return req.get_header_value("X-Real-IP");
    return req.remote_addr;
  }
  
  string getUser(const httplib::Request &req)  const
  {
    string ip=getIP(req), agent= req.get_header_value("User-Agent");
    return d_sessions.getUserForSession(getSessionID(req), agent, ip);
  }

  Sessions& d_sessions;
  Users& d_users;
};

struct AuthSentinel
{
  AuthSentinel(AuthReqs& ar, string auth) : d_ar(ar), d_auth(auth)
  {
    d_ar.auths.insert(d_auth);
  }
  ~AuthSentinel()
  {
    d_ar.auths.erase(d_auth);
  }

  AuthReqs& d_ar;
  string d_auth;
};

void checkImageOwnership(LockedSqw& lsqw, Users& u, std::string& user, std::string& imgid)
{
  if(!u.userHasCap(user, "admin")) {
    auto check = lsqw.query("select user from images, posts where images.postId = posts.id and images.id=? and user=?", {imgid, user});
    if(check.empty())
      throw std::runtime_error("Can't touch image from post that is not yours (user '"+user+"')");
  }
}

bool checkImageOwnershipBool(LockedSqw& lsqw, Users& u, std::string& user, std::string& imgid)
{
  try {    checkImageOwnership(lsqw, u, user, imgid);  }
  catch(std::exception& e) {
    cout<<e.what()<<endl;
    return false;
  }
  return true;
}

bool shouldShow(Users& u, std::string& user, unordered_map<string, MiniSQLite::outvar_t> row)
{
  // admin and owner can always see a post
  if(get<string>(row["user"]) == user || u.userHasCap(user, "admin"))
    return true;
  
  if(!get<int64_t>(row["public"]))
    return false;

  time_t pubUntil = get<int64_t>(row["publicUntilTstamp"]);
  return (!pubUntil || time(0) < pubUntil);
}

int trifectaMain(int argc, const char**argv)
{
  argparse::ArgumentParser args("serv");

  args.add_argument("db-file").help("file to read database from").default_value("trifecta.sqlite");
  args.add_argument("--html-dir").help("directory with our HTML files").default_value("./html/");
  args.add_argument("--rnd-admin-password").help("Create admin user if necessary, and set a random password").flag();
  args.add_argument("-p", "--port").help("port number to listen on").default_value(3456).scan<'i', int>();
  args.add_argument("--local-address", "-l").help("address to listen on").default_value("0.0.0.0");
  
  try {
    args.parse_args(argc, argv);
  }
  catch (const std::runtime_error& err) {
    std::cout << err.what() << std::endl;
    std::cout << args;
    std::exit(1);
  }
  fmt::print("Database is in {}\n", args.get<string>("db-file"));
  SQLiteWriter sqw(args.get<string>("db-file"),
                   {
                     {"users", {{"user", "PRIMARY KEY"}}},
                     {"posts", {{"id", "PRIMARY KEY"}, {"user", "NOT NULL REFERENCES users(user) ON DELETE CASCADE"}}},
                     {"images", {{"id", "PRIMARY KEY"}, {"postId", "NOT NULL REFERENCES posts(id) ON DELETE CASCADE"}}},
                     {"sessions", {{"id", "PRIMARY KEY"}, {"user", "NOT NULL REFERENCES users(user) ON DELETE CASCADE"}}}
                   });
  std::mutex sqwlock;
  LockedSqw lsqw{sqw, sqwlock};
  Users u(lsqw);
  
  if(args["--rnd-admin-password"] == true) {
    bool changed=false;
    string pw = makeShortID(getRandom63());

    testrunnerPw() = pw;     // for the testrunner
    
    try {
      if(u.userHasCap("admin", "admin")) {
        cout<<"Admin user existed already, updating password to: "<< pw << endl;
        u.changePassword("admin", pw);
        changed=true;
      }
    }
    catch(...) {
    }
      
    if(!changed) {
      fmt::print("Creating user admin with password: {}\n", pw);
      u.createUser("admin", pw, "", true);
    }
  }

  try {
    auto admins=lsqw.query("select user from users where admin=1");
    if(admins.empty())
      fmt::print("WARNING: No admin users are defined, try --rnd-admin-password\n");
    else {
      fmt::print("Admin users: ");
      for(auto& a: admins) 
        fmt::print("{} ", get<string>(a["user"]));
      fmt::print("\n");
    }
  }
  catch(...) {
    fmt::print("WARNING: No admin users are defined, try --rnd-admin-password\n");
  }
  
  httplib::Server svr;
  
  svr.set_exception_handler([](const auto& req, auto& res, std::exception_ptr ep) {
    string reason;
    try {
      std::rethrow_exception(ep);
    } catch (std::exception &e) {
      reason = fmt::format("An error occurred: {}", e.what());
    } catch (...) { 
      reason = "An unknown error occurred";
    }
    cout<<req.path<<": 500 created for "<<reason<<endl;
    string html = fmt::format("<html><body><h1>Error</h1>{}</body></html>", reason);
    res.set_content(html, "text/html");
    res.status = 500;
  });
  
  svr.set_mount_point("/", args.get<string>("html-dir"));
  
  Sessions sessions(lsqw);
  AuthReqs a(sessions, u);

  // anyone can do this

  svr.Post("/login", [&lsqw, &sessions, &u, &a](const httplib::Request &req, httplib::Response &res) {
    string user = req.get_file_value("user").content;
    string password = req.get_file_value("password").content;
    nlohmann::json j;
    j["ok"]=0;
    if(u.checkPassword(user, password)) {
      string ip=a.getIP(req), agent= req.get_header_value("User-Agent");
      string sessionid = sessions.createSessionForUser(user, ip, agent);
      res.set_header("Set-Cookie",
                     "session="+sessionid+"; SameSite=Strict; Path=/; Max-Age="+to_string(5*365*86400));
      cout<<"Logged in user "<<user<<endl;
      j["ok"]=1;
      j["message"]="welcome!";
      lsqw.addValue({{"action", "login"}, {"user", user}, {"ip", a.getIP(req)}, {"tstamp", time(0)}}, "log");
      lsqw.query("update users set lastLoginTstamp=? where user=?", {time(0), user});
              
    }
    else {
      cout<<"Wrong user or password"<<endl;
      j["message"]="Wrong user or password";
      lsqw.addValue({{"action", "failed-login"}, {"user", user}, {"ip", a.getIP(req)}, {"tstamp", time(0)}}, "log");
    }
    
    res.set_content(j.dump(), "application/json");
  });

  svr.Get("/join-session/:sessionid", [&lsqw, a](const auto& req, auto& res) {
    string sessionid = req.path_params.at("sessionid");
    
    res.set_header("Set-Cookie",
                   "session="+sessionid+"; SameSite=Strict; Path=/; Max-Age="+to_string(5*365*86400));
    res.set_header("Location", "../");
    res.status = 303;
  });

  svr.Get("/getPost/:postid", [&lsqw, a, &u](const auto& req, auto& res) {
    string postid = req.path_params.at("postid");
    string user;
    try {
      user = a.getUser(req);
    }catch(...){}

    nlohmann::json j;

    auto post = lsqw.query("select user, public, title, publicUntilTstamp from posts where id=?", {postid});
    if(post.size() != 1) {
      j["images"] = nlohmann::json::array();
    }
    else if(shouldShow(u, user, post[0])) {
      auto images = lsqw.query("select images.id as id, caption from images,posts where postId = ? and images.postId = posts.id", {postid});

      j["images"]=packResultsJson(images);
      j["title"]=get<string>(post[0]["title"]);
      j["public"]=get<int64_t>(post[0]["public"]);
      j["publicUntil"]=get<int64_t>(post[0]["publicUntilTstamp"]);
    }
    res.set_content(j.dump(), "application/json");
  });
  
  svr.Get("/i/:imgid", [&lsqw, a, &u](const auto& req, auto& res) {
    string imgid = req.path_params.at("imgid");
    string user;
    res.status = 404;
    
    try {
      user = a.getUser(req);
    }catch(...){}
    
    auto results = lsqw.query("select image,public,content_type, posts.publicUntilTstamp, posts.user from images,posts where images.id=? and posts.id = images.postId ", {imgid});
    
    if(results.size() != 1) {
      lsqw.addValue({{"action", "view-failed"} , {"user", user}, {"imageId", imgid}, {"ip", a.getIP(req)}, {"tstamp", time(0)}, {"meta", "no such image"}}, "log");
      return;
    }
    
    if(!shouldShow(u, user, results[0])) {
      lsqw.addValue({{"action", "view-failed"} , {"user", user}, {"imageId", imgid}, {"ip", a.getIP(req)}, {"tstamp", time(0)}}, "log");
      return;
    }
          
    auto img = get<vector<uint8_t>>(results[0]["image"]);
    string s((char*)&img[0], img.size());
    res.set_content(s, get<string>(results[0]["content_type"]));
    res.status = 200;

    lsqw.addValue({{"action", "view"} , {"user", user}, {"imageId", imgid}, {"ip", a.getIP(req)}, {"tstamp", time(0)}}, "log");
  });

  svr.Get("/status", [&lsqw, a, &u](const httplib::Request &req, httplib::Response &res) {
    nlohmann::json j;
    string user;
    try {
      user = a.getUser(req);
    }
    catch(exception& e) {
      cout<<"On /status, could not find a session"<<endl;
    }
    /*    for(auto&& [k, v] : req.headers) {
      cout<< k <<": "<<v<<endl;
    }
    */
    j["login"] = !user.empty();
    j["admin"] = false;
    if(!user.empty()) {
      j["user"] = user;
      j["admin"]=u.userHasCap(user, "admin");
    }
    
    res.set_content(j.dump(), "application/json");
  });
  

  {
    AuthSentinel as(a, "valid-user");

    svr.Post("/upload", [&lsqw, a, &u](const auto& req, auto& res) {
      if(!a.check(req))
        throw std::runtime_error("Can't upload if not logged in");
      string user = a.getUser(req);
      time_t tstamp = time(0);
      string postId = req.get_file_value("postId").content;
      if(postId.empty()) {
        postId = makeShortID(getRandom63());
        lsqw.addValue({{"id", postId}, {"user", user}, {"stamp", tstamp}, {"public", 1}, {"publicUntilTstamp", 0}, {"title", ""}}, "posts");
      }
      else if(!u.userHasCap(user, "admin")) {
        auto access=lsqw.query("select id from posts where id=? and user=?", {postId, a.getUser(req)});
        if(access.empty())
          throw std::runtime_error("Attempt to upload to post that's not ours!");
      }
      
      for(auto&& [name, f] : req.files) {
        fmt::print("name {}, filename {}, content_type {}, size {}, postid {}\n", f.name, f.filename, f.content_type, f.content.size(), postId);
        if(f.content_type.substr(0,6) != "image/" || f.filename.empty()) {
          cout<<"Skipping non-image or non-file"<<endl;
          continue;
        }
        vector<uint8_t> content(f.content.c_str(), f.content.c_str() + f.content.size());
        auto imgid=makeShortID(getRandom63());
        lsqw.addValue({{"id", imgid},
                       {"ip", a.getIP(req)},
                       {"tstamp", tstamp},
                       {"image", content},
                       {"content_type", f.content_type},
                       {"postId", postId},
                       {"caption", ""}
          }, "images");
        nlohmann::json j;
        j["id"]=imgid;
        j["postId"] = postId;
        res.set_content(j.dump(), "application/json");
        lsqw.addValue({{"action", "upload"} , {"user", a.getUser(req)}, {"imageId", imgid}, {"ip", a.getIP(req)}, {"tstamp", tstamp}}, "log");
      }
    });
    
    svr.Post("/delete-image/(.+)", [&lsqw, a, &u](const auto& req, auto& res) {
      if(!a.check(req))
        throw std::runtime_error("Can't delete if not logged in");
      string imgid = req.matches[1];

      string user = a.getUser(req);
      cout<<"Attemping to delete image "<<imgid<<" for user " << user << endl;
      checkImageOwnership(lsqw, u, user, imgid);
      
      lsqw.query("delete from images where id=?", {imgid});
      lsqw.addValue({{"action", "delete-image"}, {"ip", a.getIP(req)}, {"user", user}, {"imageId", imgid}, {"tstamp", time(0)}}, "log");
    });

    svr.Post("/set-post-title/(.+)", [&lsqw, a, &u](const auto& req, auto& res) {
      if(!a.check(req))
        throw std::runtime_error("Can't set post title if not logged in");
      string postid = req.matches[1];

      string title = req.get_file_value("title").content;
      string user = a.getUser(req);
      cout<<"Attemping to set title for post "<< postid<<" for user " << user <<" to " << title << endl;
      auto rows = lsqw.query("select user from posts where id=?", {postid});
      if(rows.size() != 1)
        throw std::runtime_error("Attempting to change title for post that does not exist");

      if(get<string>(rows[0]["user"]) != user && !u.userHasCap(user, "admin"))
         throw std::runtime_error("Attempting to change title for post that is not yours and you are not admin");
      
      lsqw.query("update posts set title=? where user=? and id=?", {title, user, postid});
      lsqw.addValue({{"action", "set-post-title"}, {"ip", a.getIP(req)}, {"user", user}, {"postId", postid}, {"tstamp", time(0)}}, "log");
    });

    svr.Post("/set-image-caption/(.+)", [&lsqw, a, &u](const auto& req, auto& res) {
      if(!a.check(req))
        throw std::runtime_error("Can't set image caption if not logged in");
      string imgid = req.matches[1];

      string caption = req.get_file_value("caption").content;
      string user = a.getUser(req);
      cout<<"Attemping to set caption for image "<< imgid<<" for user " << user <<" to " << caption << endl;
      checkImageOwnership(lsqw, u, user, imgid);
      lsqw.query("update images set caption=? where id=?", {caption, imgid});
      lsqw.addValue({{"action", "set-image-caption"}, {"ip", a.getIP(req)}, {"user", user}, {"imageId", imgid}, {"tstamp", time(0)}}, "log");
    });

    svr.Post("/change-my-password/?", [&lsqw, &u, a](const auto& req, auto& res) {
      if(!a.check(req))
        throw std::runtime_error("Can't change your password if not logged in");
      auto pwfield = req.get_file_value("password");
      if(pwfield.content.empty())
        throw std::runtime_error("Can't set an empty password");
      
      string user = a.getUser(req);
      cout<<"Attemping to set password for user "<<user<<endl;
      u.changePassword(user, pwfield.content);
    });
    
    svr.Post("/set-post-public/([^/]+)/([01])/?([0-9]*)", [&lsqw, a, &u](const auto& req, auto& res) {
      if(!a.check(req))
        throw std::runtime_error("Can't change public setting if not logged in");
      string postid = req.matches[1];
      bool pub = stoi(req.matches[2]);

      string user = a.getUser(req);
      time_t until=0;
         
      if(req.matches.size() > 3) {
        string untilStr = req.matches[3];
        if(!untilStr.empty())
          until = stoi(untilStr);
      }
      cout<<"postid: "<< postid << ", new state: "<<pub<<", until: "<<until <<", matches "<< req.matches.size()<<endl;
      if(!pub && until)
        throw std::runtime_error("Attempting to set nonsensical combination for public");

      if(until)
        lsqw.query("update posts set public = ?, publicUntilTstamp=? where id=?", {pub, until, postid});
      else
        lsqw.query("update posts set public =? where id=?", {pub, postid});
      lsqw.addValue({{"action", "change-post-public"}, {"ip", a.getIP(req)}, {"user", user}, {"postId", postid}, {"pub", pub}, {"tstamp", time(0)}}, "log");
    });

    svr.Get("/can_touch_post/:postid", [&lsqw, a, &u](const httplib::Request &req, httplib::Response &res) {
      nlohmann::json j;
      string postid = req.path_params.at("postid");

      j["can_touch_post"]=0;
      
      try {
        if(a.check(req)) {
          string user = a.getUser(req);
          auto sqres = lsqw.query("select count(1) as c from posts where id=? and user=?", {postid, user});
          if(get<int64_t>(sqres[0]["c"]) || u.userHasCap(user, "admin"))
            j["can_touch_post"]=1;
        }
      }
      catch(exception&e) { cout<<"No session for checking access rights: "<<e.what()<<"\n";}
      res.set_content(j.dump(), "application/json");
    });
    
    svr.Get("/my-images", [&lsqw, a](const httplib::Request &req, httplib::Response &res) {
      if(!a.check(req)) { // saves a 5xx error
        res.set_content("[]", "application/json");
        return;
      }
      lsqw.queryJ(res, "select images.id as id, postid, images.tstamp, content_type,length(image) as size, public, posts.publicUntilTstamp,title,caption from images,posts where postId = posts.id and user=?", {a.getUser(req)});
    });  

    svr.Post("/logout", [&lsqw, a](const httplib::Request &req, httplib::Response &res) mutable {
      if(a.check(req)) {
        lsqw.addValue({{"action", "logout"}, {"user", a.getUser(req)}, {"ip", a.getIP(req)}, {"tstamp", time(0)}}, "log");
        a.dropSession(req);
        res.set_header("Set-Cookie",
                       "session="+a.getSessionID(req)+"; SameSite=Strict; Path=/; Max-Age=0");
      }
    });
    // ponder adding logout-everywhere
    
    {
      AuthSentinel as(a, "admin");
      svr.Get("/all-images", [&lsqw, a](const httplib::Request &req, httplib::Response &res) {
        if(!a.check(req)) {
          throw std::runtime_error("Not admin");
        }
        lsqw.queryJ(res, "select images.id as id, postId, user,tstamp,content_type,length(image) as size, posts.public, ip from images,posts where posts.id=images.postId", {});
      });

      svr.Get("/all-users", [&lsqw, a](const httplib::Request &req, httplib::Response &res) {
        if(!a.check(req)) {
          throw std::runtime_error("Not admin");
        }
        lsqw.queryJ(res, "select user, email, disabled, lastLoginTstamp, admin from users", {});
      });

      svr.Get("/all-sessions", [&lsqw, a](const httplib::Request &req, httplib::Response &res) {
        if(!a.check(req)) {
          throw std::runtime_error("Not admin");
        }
        lsqw.queryJ(res, "select * from sessions", {});
      });
      
      svr.Post("/create-user", [&lsqw, &sessions, &u, a](const httplib::Request &req, httplib::Response &res) {
        if(!a.check(req)) {
          throw std::runtime_error("Not admin");
        }

        string password1 = req.get_file_value("password1").content;
        string user = req.get_file_value("user").content;
        nlohmann::json j;
        
        if(password1.empty() || user.empty()) {
          j["ok"]=false;
          j["message"] = "User or password field empty";
        }
        else {
          try {
            u.createUser(user, password1, "", false);
            j["ok"] = true;
          }
          catch(std::exception& e) {
            j["ok"]=false;
            j["message"]=e.what();
          }
        }
        res.set_content(j.dump(), "application/json");
      });
    }

    svr.Post("/change-user-disabled/([^/]+)/([01])", [&lsqw, a](const auto& req, auto& res) {
      if(!a.check(req))
        throw std::runtime_error("Not admin");
      string user = req.matches[1];
      bool disabled = stoi(req.matches[2]);
      lsqw.query("update users set disabled = ? where user=?", {disabled, user});
      if(disabled) {
        lsqw.query("delete from sessions where user=?", {user});
      }
      lsqw.addValue({{"action", "change-user-disabled"}, {"user", user}, {"ip", a.getIP(req)}, {"disabled", disabled}, {"tstamp", time(0)}}, "log");
    });

    svr.Post("/change-password/?", [&lsqw, &u, a](const auto& req, auto& res) {
      if(!a.check(req))
        throw std::runtime_error("Not admin");
      auto pwfield = req.get_file_value("password");
      if(pwfield.content.empty())
        throw std::runtime_error("Can't set an empty password");
      
      string user = req.get_file_value("user").content;
      cout<<"Attemping to set password for user "<<user<<endl;
      u.changePassword(user, pwfield.content);
    });
    
    svr.Post("/kill-session/([^/]+)", [&lsqw, a](const auto& req, auto& res) {
      if(!a.check(req))
        throw std::runtime_error("Not admin");
      string session = req.matches[1];
      lsqw.query("delete from sessions where id=?", {session});
      lsqw.addValue({{"action", "kill-session"}, {"ip", a.getIP(req)}, {"session", session}, {"tstamp", time(0)}}, "log");
    });

    svr.Post("/del-user/([^/]+)", [&lsqw, a, &u](const auto& req, auto& res) {
      if(!a.check(req))
        throw std::runtime_error("Not admin");
      string user = req.matches[1];
      u.delUser(user);

      lsqw.addValue({{"action", "del-user"}, {"ip", a.getIP(req)}, {"user", user}, {"tstamp", time(0)}}, "log");
    });

    svr.Post("/stop" , [&lsqw, a, &svr](const auto& req, auto& res) {
      if(!a.check(req))
        throw std::runtime_error("Not admin");
      lsqw.addValue({{"action", "stop"}, {"ip", a.getIP(req)}, {"user", a.getUser(req)}, {"tstamp", time(0)}}, "log");

      cout<<"Attempting to stop server"<<endl;
      svr.stop();
    });
  }

  string laddr = args.get<string>("local-address");
  cout<<"Will listen on http://"<< laddr <<":"<<args.get<int>("port")<<endl;
  
  svr.set_socket_options([](socket_t sock) {
   int yes = 1;
   setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
                reinterpret_cast<const void *>(&yes), sizeof(yes));
  });

  if(!svr.listen(laddr, args.get<int>("port"))) {
    cout<<"Error launching server: "<<strerror(errno)<<endl;
    return EXIT_FAILURE;
  }
  cout<<"Stopping"<<endl;
  return 0;
}
