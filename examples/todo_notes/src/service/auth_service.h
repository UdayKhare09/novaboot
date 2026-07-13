#pragma once
#include "repository/app_user_repository.h"
#include "model/dto.h"
#include "novaboot/middleware/jwt_middleware.h"
#include <openssl/evp.h>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <random>
#include <sstream>
#include <iomanip>

namespace todo_notes::service {

using todo_notes::model::AppUser;
using todo_notes::model::RegisterRequest;
using todo_notes::model::LoginRequest;
using todo_notes::model::LoginResponse;
using novaboot::middleware::JwtIssuer;
using novaboot::middleware::JwtTokenBuilder;

struct AuthService {
    AppUserRepository& user_repo;

    explicit AuthService(AppUserRepository& repo) : user_repo(repo) {}

    std::string generate_uuid() {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<> dis(0, 15);
        static std::uniform_int_distribution<> dis2(8, 11);

        std::stringstream ss;
        ss << std::hex;
        for (int i = 0; i < 8; i++) ss << dis(gen);
        ss << "-";
        for (int i = 0; i < 4; i++) ss << dis(gen);
        ss << "-4";
        for (int i = 0; i < 3; i++) ss << dis(gen);
        ss << "-";
        ss << dis2(gen);
        for (int i = 0; i < 3; i++) ss << dis(gen);
        ss << "-";
        for (int i = 0; i < 12; i++) ss << dis(gen);
        return ss.str();
    }

    std::string hash_password(const std::string& password, const std::string& salt) {
        std::string salted = password + ":" + salt;
        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int length = 0;
        
        EVP_MD_CTX* context = EVP_MD_CTX_new();
        if (context != nullptr) {
            if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr) &&
                EVP_DigestUpdate(context, salted.c_str(), salted.length()) &&
                EVP_DigestFinal_ex(context, hash, &length)) {
                // Success
            }
            EVP_MD_CTX_free(context);
        }
        
        std::string out;
        out.reserve(length * 2);
        for (unsigned int i = 0; i < length; ++i) {
            char buf[3];
            snprintf(buf, sizeof(buf), "%02x", hash[i]);
            out += buf;
        }
        return out;
    }

    AppUser register_user(const RegisterRequest& req) {
        if (user_repo.find_by_username(req.username).has_value()) {
            throw std::runtime_error("Username already exists");
        }
        if (user_repo.find_by_email(req.email).has_value()) {
            throw std::runtime_error("Email already registered");
        }

        AppUser user;
        user.id = generate_uuid();
        user.username = req.username;
        user.email = req.email;
        user.password_hash = hash_password(req.password, req.username);

        return user_repo.save(user);
    }

    LoginResponse login_user(const LoginRequest& req) {
        auto user_opt = user_repo.find_by_username(req.username);
        if (!user_opt) {
            throw std::runtime_error("Invalid username or password");
        }

        auto expected_hash = hash_password(req.password, req.username);
        if (user_opt->password_hash != expected_hash) {
            throw std::runtime_error("Invalid username or password");
        }

        // Issue JWT token with 24 hours expiration
        JwtIssuer issuer(JwtIssuer::Config{
            .algorithm = JwtIssuer::Algorithm::HS256,
            .hmac_secret = "sample-secret",
            .default_ttl = std::chrono::hours{24}
        });

        JwtTokenBuilder builder;
        builder.subject(user_opt->username)
               .issuer("novaboot-sample")
               .audience("sample-api")
               .scopes({"read", "write"})
               .claim("user_id", user_opt->id)
               .issued_now();

        auto issued = issuer.issue(builder);
        if (!issued.has_value()) {
            throw std::runtime_error("Failed to issue JWT token: " + issued.error());
        }

        return LoginResponse{ .token = *issued };
    }
};

} // namespace todo_notes::service
