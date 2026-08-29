#include "apprequest.h"

// name/args are nanopb pointer-callback strings: the message holds a char*
// that points straight at our QByteArray's bytes, so the QByteArrays must
// outlive the encode -- which they do, being members of this request object.
AppStartRequest::AppStartRequest(uint32_t id, const QByteArray &name, const QByteArray &args):
    MainRequest(id, PB_Main_app_start_request_tag),
    m_name(name),
    m_args(args)
{
    auto &content = m_message.content.app_start_request;
    content.name = m_name.data();
    content.args = m_args.isEmpty() ? nullptr : m_args.data();
}

AppExitRequest::AppExitRequest(uint32_t id):
    MainRequest(id, PB_Main_app_exit_request_tag)
{}
