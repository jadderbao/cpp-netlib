#ifndef BOOST_NETWORK_PROTOCOL_HTTP_IMPL_HTTP_ASYNC_PROTOCOL_HANDLER_HPP_
#define BOOST_NETWORK_PROTOCOL_HTTP_IMPL_HTTP_ASYNC_PROTOCOL_HANDLER_HPP_

// Copyright 2010 (C) Dean Michael Berris
// Copyright 2011 Dean Michael Berris (dberris@google.com).
// Copyright 2011 Google, Inc.
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#include <boost/fusion/tuple/tuple.hpp>
#include <boost/fusion/tuple/tuple_tie.hpp>
#include <boost/logic/tribool.hpp>
#include <boost/network/detail/debug.hpp>
#include <boost/network/protocol/http/algorithms/linearize.hpp>
#include <boost/network/protocol/http/parser/incremental.hpp>
#include <boost/network/protocol/http/request_parser.hpp>
#include <boost/network/traits/string.hpp>

namespace boost {
namespace network {
namespace http {
namespace impl {

template <class Tag, unsigned version_major, unsigned version_minor>
struct http_async_protocol_handler {
 protected:
  typedef typename string<Tag>::type string_type;

#ifdef BOOST_NETWORK_DEBUG
  struct debug_escaper {
    string_type& string_;
    explicit debug_escaper(string_type& string) : string_(string) {}
    debug_escaper(debug_escaper const&) = default;
    debug_escaper(debug_escaper&&) noexcept = default;
    void operator()(typename string_type::value_type input) {
      if (!algorithm::is_print()(input)) {
        typename ostringstream<Tag>::type escaped_stream;
        if (input == '\r') {
          string_.append("\\r");
        } else if (input == '\n') {
          string_.append("\\n");
        } else {
          escaped_stream << "\\x" << static_cast<int>(input);
          string_.append(escaped_stream.str());
        }
      } else {
        string_.push_back(input);
      }
    }
  };
#endif

  template <class ResponseType>
  void init_response(ResponseType& response_, bool get_body) {
    // TODO(dberris): review parameter necessity.
    (void)get_body;

    boost::shared_future<string_type> source_future(
        source_promise.get_future());
    source(response_, source_future);

    boost::shared_future<string_type> destination_future(
        destination_promise.get_future());
    destination(response_, destination_future);

    boost::shared_future<typename headers_container<Tag>::type> headers_future(
        headers_promise.get_future());
    headers(response_, headers_future);

    boost::shared_future<string_type> body_future(body_promise.get_future());
    body(response_, body_future);

    boost::shared_future<string_type> version_future(
        version_promise.get_future());
    version(response_, version_future);

    boost::shared_future<boost::uint16_t> status_future(
        status_promise.get_future());
    status(response_, status_future);

    boost::shared_future<string_type> status_message_future(
        status_message_promise.get_future());
    status_message(response_, status_message_future);
  }

  struct to_http_headers {
    typedef typename string<Tag>::type string_type;
    template <class U>
    string_type const operator()(U const& pair) const {
      typedef typename ostringstream<Tag>::type ostringstream_type;
      typedef constants<Tag> constants;
      ostringstream_type header_line;
      header_line << pair.first << constants::colon() << constants::space()
                  << pair.second << constants::crlf();
      return header_line.str();
    }
  };

  template <class Delegate, class Callback>
  logic::tribool parse_version(Delegate& delegate_, Callback callback,
                               size_t bytes) {
    logic::tribool parsed_ok;
    part_begin = part.begin();
    typename buffer_type::const_iterator part_end = part.begin();
    std::advance(part_end, bytes);
    typename boost::iterator_range<typename buffer_type::const_iterator>
        result_range,
        input_range = boost::make_iterator_range(part_begin, part_end);
    fusion::tie(parsed_ok, result_range) = response_parser_.parse_until(
        response_parser_type::http_version_done, input_range);
    if (parsed_ok == true) {
      string_type version;
      std::swap(version, partial_parsed);
      version.append(boost::begin(result_range), boost::end(result_range));
      algorithm::trim(version);
      version_promise.set_value(version);
      part_begin = boost::end(result_range);
    } else if (parsed_ok == false) {
#ifdef BOOST_NETWORK_DEBUG
      string_type escaped;
      debug_escaper escaper(escaped);
      std::for_each(part_begin, part_end, escaper);
      BOOST_NETWORK_MESSAGE("[parser:" << response_parser_.state()
                                       << "] buffer contents: \"" << escaped
                                       << "\"");
#endif
      std::runtime_error error("Invalid Version Part.");
      version_promise.set_exception(boost::copy_exception(error));
      status_promise.set_exception(boost::copy_exception(error));
      status_message_promise.set_exception(boost::copy_exception(error));
      headers_promise.set_exception(boost::copy_exception(error));
      source_promise.set_exception(boost::copy_exception(error));
      destination_promise.set_exception(boost::copy_exception(error));
      body_promise.set_exception(boost::copy_exception(error));
    } else {
      partial_parsed.append(boost::begin(result_range),
                            boost::end(result_range));
      part_begin = part.begin();
      delegate_->read_some(
          boost::asio::mutable_buffers_1(part.c_array(), part.size()),
          callback);
    }
    return parsed_ok;
  }

  template <class Delegate, class Callback>
  logic::tribool parse_status(Delegate& delegate_, Callback callback,
                              size_t bytes) {
    logic::tribool parsed_ok;
    typename buffer_type::const_iterator part_end = part.begin();
    std::advance(part_end, bytes);
    typename boost::iterator_range<typename buffer_type::const_iterator>
        result_range,
        input_range = boost::make_iterator_range(part_begin, part_end);
    fusion::tie(parsed_ok, result_range) = response_parser_.parse_until(
        response_parser_type::http_status_done, input_range);
    if (parsed_ok == true) {
      string_type status;
      std::swap(status, partial_parsed);
      status.append(boost::begin(result_range), boost::end(result_range));
      trim(status);
      boost::uint16_t status_int = lexical_cast<boost::uint16_t>(status);
      status_promise.set_value(status_int);
      part_begin = boost::end(result_range);
    } else if (parsed_ok == false) {
#ifdef BOOST_NETWORK_DEBUG
      string_type escaped;
      debug_escaper escaper(escaped);
      std::for_each(part_begin, part_end, escaper);
      BOOST_NETWORK_MESSAGE("[parser:" << response_parser_.state()
                                       << "] buffer contents: \"" << escaped
                                       << "\"");
#endif
      std::runtime_error error("Invalid status part.");
      status_promise.set_exception(boost::copy_exception(error));
      status_message_promise.set_exception(boost::copy_exception(error));
      headers_promise.set_exception(boost::copy_exception(error));
      source_promise.set_exception(boost::copy_exception(error));
      destination_promise.set_exception(boost::copy_exception(error));
      body_promise.set_exception(boost::copy_exception(error));
    } else {
      partial_parsed.append(boost::begin(result_range),
                            boost::end(result_range));
      part_begin = part.begin();
      delegate_->read_some(
          boost::asio::mutable_buffers_1(part.c_array(), part.size()),
          callback);
    }
    return parsed_ok;
  }

  template <class Delegate, class Callback>
  logic::tribool parse_status_message(Delegate& delegate_, Callback callback,
                                      size_t bytes) {
    logic::tribool parsed_ok;
    typename buffer_type::const_iterator part_end = part.begin();
    std::advance(part_end, bytes);
    typename boost::iterator_range<typename buffer_type::const_iterator>
        result_range,
        input_range = boost::make_iterator_range(part_begin, part_end);
    fusion::tie(parsed_ok, result_range) = response_parser_.parse_until(
        response_parser_type::http_status_message_done, input_range);
    if (parsed_ok == true) {
      string_type status_message;
      std::swap(status_message, partial_parsed);
      status_message.append(boost::begin(result_range),
                            boost::end(result_range));
      algorithm::trim(status_message);
      status_message_promise.set_value(status_message);
      part_begin = boost::end(result_range);
    } else if (parsed_ok == false) {
#ifdef BOOST_NETWORK_DEBUG
      string_type escaped;
      debug_escaper escaper(escaped);
      std::for_each(part_begin, part_end, escaper);
      BOOST_NETWORK_MESSAGE("[parser:" << response_parser_.state()
                                       << "] buffer contents: \"" << escaped
                                       << "\"");
#endif
      std::runtime_error error("Invalid status message part.");
      status_message_promise.set_exception(boost::copy_exception(error));
      headers_promise.set_exception(boost::copy_exception(error));
      source_promise.set_exception(boost::copy_exception(error));
      destination_promise.set_exception(boost::copy_exception(error));
      body_promise.set_exception(boost::copy_exception(error));
    } else {
      partial_parsed.append(boost::begin(result_range),
                            boost::end(result_range));
      part_begin = part.begin();
      delegate_->read_some(
          boost::asio::mutable_buffers_1(part.c_array(), part.size()),
          callback);
    }
    return parsed_ok;
  }

  void parse_headers_real(string_type& headers_part) {
    typename boost::iterator_range<typename string_type::const_iterator>
        input_range = boost::make_iterator_range(headers_part),
        result_range;
    logic::tribool parsed_ok;
    response_parser_type headers_parser(
        response_parser_type::http_header_line_done);
    typename headers_container<Tag>::type headers;
    std::pair<string_type, string_type> header_pair;
    while (!boost::empty(input_range)) {
      fusion::tie(parsed_ok, result_range) = headers_parser.parse_until(
          response_parser_type::http_header_colon, input_range);
      if (headers_parser.state() != response_parser_type::http_header_colon)
        break;
      header_pair.first =
          string_type(boost::begin(result_range), boost::end(result_range));
      input_range.advance_begin(boost::distance(result_range));
      fusion::tie(parsed_ok, result_range) = headers_parser.parse_until(
          response_parser_type::http_header_line_done, input_range);
      header_pair.second =
          string_type(boost::begin(result_range), boost::end(result_range));
      input_range.advance_begin(boost::distance(result_range));

      trim(header_pair.first);
      if (header_pair.first.size() > 1) {
        header_pair.first.erase(header_pair.first.size() - 1);
      }
      trim(header_pair.second);
      headers.insert(header_pair);
    }
    // determine if the body parser will need to handle chunked encoding
    typename headers_range<basic_response<Tag> >::type transfer_encoding_range =
        headers.equal_range("Transfer-Encoding");
    is_chunk_encoding =
        !boost::empty(transfer_encoding_range) &&
        boost::iequals(boost::begin(transfer_encoding_range)->second,
                       "chunked");
    headers_promise.set_value(headers);
  }

  template <class Delegate, class Callback>
  fusion::tuple<logic::tribool, size_t> parse_headers(Delegate& delegate_,
                                                      Callback callback,
                                                      size_t bytes) {
    logic::tribool parsed_ok;
    typename buffer_type::const_iterator part_end = part.begin();
    std::advance(part_end, bytes);
    typename boost::iterator_range<typename buffer_type::const_iterator>
        result_range,
        input_range = boost::make_iterator_range(part_begin, part_end);
    fusion::tie(parsed_ok, result_range) = response_parser_.parse_until(
        response_parser_type::http_headers_done, input_range);
    if (parsed_ok == true) {
      string_type headers_string;
      std::swap(headers_string, partial_parsed);
      headers_string.append(boost::begin(result_range),
                            boost::end(result_range));
      part_begin = boost::end(result_range);
      this->parse_headers_real(headers_string);
    } else if (parsed_ok == false) {
// We want to output the contents of the buffer that caused
// the error in debug builds.
#ifdef BOOST_NETWORK_DEBUG
      string_type escaped;
      debug_escaper escaper(escaped);
      std::for_each(part_begin, part_end, escaper);
      BOOST_NETWORK_MESSAGE("[parser:" << response_parser_.state()
                                       << "] buffer contents: \"" << escaped
                                       << "\" consumed length: "
                                       << boost::distance(result_range));
#endif
      std::runtime_error error("Invalid header part.");
      headers_promise.set_exception(boost::copy_exception(error));
      body_promise.set_exception(boost::copy_exception(error));
      source_promise.set_exception(boost::copy_exception(error));
      destination_promise.set_exception(boost::copy_exception(error));
    } else {
      partial_parsed.append(boost::begin(result_range),
                            boost::end(result_range));
      part_begin = part.begin();
      delegate_->read_some(
          boost::asio::mutable_buffers_1(part.c_array(), part.size()),
          callback);
    }
    return fusion::make_tuple(
        parsed_ok, std::distance(boost::end(result_range), part_end));
  }

  template <class Delegate, class Callback>
  void parse_body(Delegate& delegate_, Callback callback, size_t bytes) {
    // TODO(dberris): we should really not use a string for the partial body
    // buffer.
    partial_parsed.append(part_begin, bytes);
    part_begin = part.begin();
    delegate_->read_some(
        boost::asio::mutable_buffers_1(part.c_array(), part.size()), callback);
  }

  typedef response_parser<Tag> response_parser_type;
  // TODO(dberris): make 1024 go away and become a configurable value.
  typedef boost::array<typename char_<Tag>::type, 1024> buffer_type;

  response_parser_type response_parser_;
  boost::promise<string_type> version_promise;
  boost::promise<boost::uint16_t> status_promise;
  boost::promise<string_type> status_message_promise;
  boost::promise<typename headers_container<Tag>::type> headers_promise;
  boost::promise<string_type> source_promise;
  boost::promise<string_type> destination_promise;
  boost::promise<string_type> body_promise;
  buffer_type part;
  typename buffer_type::const_iterator part_begin;
  string_type partial_parsed;
  bool is_chunk_encoding;
};

}  // namespace impl
}  // namespace http
}  // namespace network
}  // namespace boost

#endif  // BOOST_NETWORK_PROTOCOL_HTTP_IMPL_HTTP_ASYNC_PROTOCOL_HANDLER_HPP_20101015 
