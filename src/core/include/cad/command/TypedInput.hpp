#pragma once

#include <optional>
#include <span>
#include <string_view>

#include "cad/command/Command.hpp"
#include "cad/db/Handle.hpp"

namespace cad {

/// The keyword a typed word names, or null.
///
/// An exact shortcut wins over a prefix, so at a prompt offering `Close` and
/// `Center` the drafter who types `C` gets whichever declared `C` as its
/// shortcut rather than whichever happens to be listed first.
[[nodiscard]] const CommandKeyword* matchKeyword(const InputRequest& request,
                                                 std::string_view text);

/// Turn typed text into the input a request is asking for.
///
/// This is the command *language*, not the user interface: the same words mean
/// the same thing typed at the command line, replayed from a script, or
/// proposed by an agent. It lived in the command line widget, which meant
/// anything that was not that widget had to reimplement it -- and a second
/// implementation of "what does @30,40 mean" is a second answer waiting to
/// disagree with the first.
///
/// Empty text is an answer in its own right: Enter accepts a default or ends an
/// open command, and at a selection prompt it means "that is the set", which is
/// why the current selection is passed in.
///
/// Returns nothing when the text does not name anything the request can use.
/// The caller reports that; this does not guess.
[[nodiscard]] std::optional<InputValue> parseTypedInput(
    const InputRequest& request, std::string_view text,
    std::span<const Handle> selection = {});

}  // namespace cad
