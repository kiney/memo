import json

class Base:

  pass

class NoOp(Base):

  def send_one(self, template, recipient_email, *args, **kwargs):
    pass

  def send_template(self, template, recipient_email, *args, **kwargs):
    pass

class SendWithUs(Base):

  def __init__(self, api_key):
    import sendwithus
    import sendwithus.encoder
    class JSONEncoder(sendwithus.encoder.SendwithusJSONEncoder):
      def default(self, obj):
        import bson
        import datetime
        if isinstance(obj, datetime.datetime):
          return {
            'year': obj.year,
            'month': obj.month,
            'day': obj.day,
            'hour': obj.hour,
            'minute': obj.minute,
            'second': obj.second,
            'weekday': obj.weekday(),
          }
        if isinstance(obj, bson.ObjectId):
          return str(obj)
        return super().default(obj)
    self.__json_encoder = JSONEncoder
    self.__swu = sendwithus.api(
      api_key = api_key,
      json_encoder = self.__json_encoder)
    self.__load_templates()

  def __load_templates(self):
    templates = json.loads(self.__swu.templates().content.decode())
    self.__templates = dict((t['id'], t) for t in templates)

  def __execute(self, batch):
    r = batch.execute()
    if r.status_code != 200:
      try:
        print('%s: send with us status code: %s' % (self, r.json()), file = sys.stderr)
      except Exception as e:
        print('%s: non-JSON response (%s): %s' % (self, e, r.text), file = sys.stderr)
      raise Exception('%s: request failed' % self)
    return r

  def __template(self, name):
    if name not in self.__templates:
      self.__load_templates()
    if name not in self.__templates:
      raise Exception('no such template: %s' % name)
    return self.__templates[name]['id']

  def send_one(self,
               template,
               recipient_email,
               recipient_name = None,
               sender_email = None,
               sender_name = None,
               reply_to = None,
               variables = None,
               ):
    return self.__send_one(self.__template(template),
                           recipient_email,
                           recipient_name,
                           sender_email,
                           sender_name,
                           reply_to,
                           variables,
                           self.__swu,
                         )

  def __send_one(self,
                 template,
                 recipient_email,
                 recipient_name,
                 sender_email,
                 sender_name,
                 reply_to,
                 variables,
                 swu,
               ):
    sender = None
    if any(x is not None for x in (sender_email, sender_name)):
      sender = {}
      if sender_email is not None:
        sender['address'] = sender_email
      if sender_name is not None:
        sender['name'] = sender_name
      if reply_to is not None:
        sender['reply_to'] = reply_to
    recipient = {
      'address': recipient_email,
    }
    if recipient_name is not None:
      recipient['name'] = recipient_name
    swu.send(
      email_id = template,
      recipient = recipient,
      sender = sender,
      email_data = variables,
    )

  def send_template(self, template, recipients):
    template = self.__template(template)
    swu = self.__swu.start_batch()
    for recipient in recipients:
      email = recipient['email']
      sender_name = None
      sender_email = None
      reply_to = None
      if recipient.get('sender') is not None:
        sender_name = recipient['sender'].get('fullname')
        sender_email = recipient['sender'].get('email')
        reply_to = recipient['sender'].get('reply-to')
      self.__send_one(
        template = template,
        recipient_email = email,
        recipient_name = recipient['name'],
        sender_name = sender_name,
        sender_email = sender_email,
        reply_to = reply_to,
        variables = recipient['vars'],
        swu = swu)
      if swu.command_length() >= 100:
        self.__execute(swu)
    if swu.command_length() > 0:
      return self.__execute(swu)