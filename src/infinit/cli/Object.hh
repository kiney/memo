#pragma once

#include <elle/attribute.hh>

#include <elle/das/bound-method.hh>
#include <elle/das/cli.hh>

#include <infinit/cli/fwd.hh>
#include <infinit/cli/symbols.hh>

namespace infinit
{
  namespace cli
  {
    template <typename Self, typename Owner = Memo>
    class Object;

    template <typename Self, typename Owner>
    class Object
      : public elle::das::named::Function<void (decltype(help = false))>
    {
    public:
      Object(Memo& memo);
      void
      help(std::ostream& s);
      void
      call(bool help);
      void
      apply(Memo& cli, std::vector<std::string>& args);
      template <typename Symbol, typename ... Args>
      static
      auto
      binding(Symbol const&, Args&& ... args)
        -> decltype(elle::das::named::function(
                      elle::das::bind_method<Symbol, Self>(std::declval<Self&>()),
                      std::forward<Args>(args)...));
      template <typename Symbol, typename ... Args>
      auto
      bind(Symbol const& s, Args&& ... args)
        -> decltype(binding(s, std::forward<Args>(args)...));
      ELLE_ATTRIBUTE_R(Memo&, cli);
      ELLE_ATTRIBUTE_R(elle::das::cli::Options, options, protected);
    };

    template <typename Symbol, typename Object>
    struct mode_call
    {
      using type = bool;
      static
      bool
      value(infinit::cli::Memo& infinit,
            Object& o,
            std::vector<std::string>& args,
            bool& found)
      {
        if (!found && elle::das::cli::option_name_from_c(Symbol::name()) == args[0])
        {
          found = true;
          args.erase(args.begin());
          Symbol::attr_get(o).apply(infinit, args);
          return true;
        }
        else
          return false;
      }
    };
  }
}
