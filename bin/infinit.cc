#include <iostream>

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/program_options.hpp>

#include <elle/Error.hh>
#include <elle/printf.hh>
#include <elle/serialization/Serializer.hh>
#include <elle/serialization/json.hh>

#include <cryptography/KeyPair.hh>

#include <reactor/scheduler.hh>

#include <infinit/filesystem/filesystem.hh>
#include <infinit/model/faith/Faith.hh>
#include <infinit/model/paranoid/Paranoid.hh>
#include <infinit/model/Model.hh>
#include <infinit/storage/Async.hh>
#include <infinit/storage/Filesystem.hh>
#include <infinit/storage/Memory.hh>
#include <infinit/storage/Storage.hh>
#include <infinit/version.hh>

ELLE_LOG_COMPONENT("infinit");

/*----------------------.
| Storage configuration |
`----------------------*/

namespace infinit
{
  namespace storage
  {

    struct MemoryStorageConfig:
      public StorageConfig
    {
    public:
      MemoryStorageConfig(elle::serialization::SerializerIn& input)
        : StorageConfig()
      {
        this->serialize(input);
      }

      void
      serialize(elle::serialization::Serializer& s)
      {}

      virtual
      std::unique_ptr<infinit::storage::Storage>
      make() override
      {
        return elle::make_unique<infinit::storage::Memory>();
      }
    };
    static const elle::serialization::Hierarchy<StorageConfig>::
    Register<MemoryStorageConfig> _register_MemoryStorageConfig("memory");

    struct FilesystemStorageConfig:
      public StorageConfig
    {
    public:
      std::string path;

      FilesystemStorageConfig(elle::serialization::SerializerIn& input)
        : StorageConfig()
      {
        this->serialize(input);
      }

      void
      serialize(elle::serialization::Serializer& s)
      {
        s.serialize("path", this->path);
      }

      virtual
      std::unique_ptr<infinit::storage::Storage>
      make() override
      {
        return elle::make_unique<infinit::storage::Filesystem>(this->path);
      }
    };
    static const elle::serialization::Hierarchy<StorageConfig>::
    Register<FilesystemStorageConfig>
    _register_FilesystemStorageConfig("filesystem");
  }
}


/*--------------------.
| Model configuration |
`--------------------*/

struct ModelConfig:
  public elle::serialization::VirtuallySerializable
{
  static constexpr char const* virtually_serializable_key = "type";

  virtual
  std::unique_ptr<infinit::model::Model>
  make() = 0;
};

struct FaithModelConfig:
  public ModelConfig
{
public:
  std::unique_ptr<infinit::storage::StorageConfig> storage;

  FaithModelConfig(elle::serialization::SerializerIn& input)
    : ModelConfig()
  {
    this->serialize(input);
  }

  void
  serialize(elle::serialization::Serializer& s)
  {
    s.serialize("storage", this->storage);
  }

  virtual
  std::unique_ptr<infinit::model::Model>
  make()
  {
    return elle::make_unique<infinit::model::faith::Faith>
      (this->storage->make());
  }
};
static const elle::serialization::Hierarchy<ModelConfig>::
Register<FaithModelConfig> _register_FaithModelConfig("faith");

struct ParanoidModelConfig:
  public ModelConfig
{
public:
  // boost::optional does not support in-place construction, use a
  // std::unique_ptr instead since KeyPair is not copiable.
  std::unique_ptr<infinit::cryptography::KeyPair> keys;
  std::unique_ptr<infinit::storage::StorageConfig> storage;

  ParanoidModelConfig(elle::serialization::SerializerIn& input)
    : ModelConfig()
  {
    this->serialize(input);
  }

  void
  serialize(elle::serialization::Serializer& s)
  {
    s.serialize("keys", this->keys);
    s.serialize("storage", this->storage);
  }

  virtual
  std::unique_ptr<infinit::model::Model>
  make()
  {
    if (!this->keys)
    {
      this->keys.reset(
        new infinit::cryptography::KeyPair(
          infinit::cryptography::KeyPair::generate
          (infinit::cryptography::Cryptosystem::rsa, 2048)));
      elle::serialization::json::SerializerOut output(std::cout);
      std::cout << "No key specified, generating fresh ones:" << std::endl;
      this->keys->serialize(output);
    }
    return elle::make_unique<infinit::model::paranoid::Paranoid>
      (std::move(*this->keys), this->storage->make());
  }
};
static const elle::serialization::Hierarchy<ModelConfig>::
Register<ParanoidModelConfig> _register_ParanoidModelConfig("paranoid");

/*--------------.
| Configuration |
`--------------*/

struct Config
{
public:
  std::string mountpoint;
  boost::optional<elle::Buffer> root_address;
  std::shared_ptr<ModelConfig> model;
  boost::optional<bool> single_mount;

  Config()
    : mountpoint()
    , model()
  {}

  Config(elle::serialization::SerializerIn& input)
    : Config()
  {
    this->serialize(input);
  }

  void
  serialize(elle::serialization::Serializer& s)
  {
    s.serialize("mountpoint", this->mountpoint);
    s.serialize("root_address", this->root_address);
    s.serialize("model", this->model);
    s.serialize("caching", this->single_mount);
  }
};

static
void
parse_options(int argc, char** argv, Config& cfg)
{
  ELLE_TRACE_SCOPE("parse command line");
  using namespace boost::program_options;
  options_description options("Options");
  options.add_options()
    ("config,c", value<std::string>(), "configuration file")
    ("help,h", "display the help")
    ("version,v", "display version")
    ;
  variables_map vm;
  try
  {
    store(parse_command_line(argc, argv, options), vm);
    notify(vm);
  }
  catch (invalid_command_line_syntax const& e)
  {
    throw elle::Error(elle::sprintf("command line error: %s", e.what()));
  }
  if (vm.count("help"))
  {
    std::cout << "Usage: " << argv[0] << " [options]" << std::endl;
    std::cout << std::endl;
    std::cout << options;
    std::cout << std::endl;
    std::cout << "Infinit v" << INFINIT_VERSION << std::endl;
    exit(0); // XXX: use Exit exception
  }
  if (vm.count("version"))
  {
    std::cout << INFINIT_VERSION << std::endl;
    exit(0); // XXX: use Exit exception
  }
  if (vm.count("config") != 0)
  {
    std::string const config = vm["config"].as<std::string>();
    boost::filesystem::ifstream input_file(config);
    try
    {
      elle::serialization::json::SerializerIn input(input_file);
      cfg.serialize(input);
    }
    catch (elle::serialization::Error const& e)
    {
      throw elle::Error(
        elle::sprintf("error in configuration file %s: %s", config, e.what()));
    }
  }
  else
    throw elle::Error("missing mandatory 'config' option");
}

int
main(int argc, char** argv)
{
  try
  {
    reactor::Scheduler sched;
    reactor::Thread main(
      sched,
      "main",
      [argc, argv]
      {
        Config cfg;
        parse_options(argc, argv, cfg);
        auto model = cfg.model->make();
        std::unique_ptr<infinit::filesystem::FileSystem> fs;
        ELLE_TRACE("initialize filesystem")
          if (cfg.root_address)
          {
            using infinit::model::Address;
            auto root = elle::serialization::Serialize<Address>::
              convert(cfg.root_address.get());
            fs = elle::make_unique<infinit::filesystem::FileSystem>
              (root, std::move(model));
          }
          else
          {
            fs = elle::make_unique<infinit::filesystem::FileSystem>(
              std::move(model));
            std::cout << "No root block specified, generating fresh one:"
                      << std::endl;
            elle::serialization::json::SerializerOut output(std::cout);
            output.serialize_forward(fs->root_address());
          }
        if (cfg.single_mount && *cfg.single_mount)
          fs->single_mount(true);
        ELLE_TRACE("mount filesystem")
        {
          reactor::filesystem::FileSystem filesystem(std::move(fs), true);
          filesystem.mount(cfg.mountpoint, {});
          reactor::scheduler().signal_handle(SIGINT, [&] { filesystem.unmount();});
          ELLE_TRACE("Waiting on filesystem");
          reactor::wait(filesystem);
          ELLE_TRACE("Filesystem finished.");
        }
      });
    sched.run();
  }
  catch (std::exception const& e)
  {
    elle::fprintf(std::cerr, "%s: fatal error: %s\n", argv[0], e.what());
    return 1;
  }
}
