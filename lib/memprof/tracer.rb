begin
  require File.expand_path('../../memprof', __FILE__)
rescue LoadError
  require File.expand_path('../../../ext/memprof', __FILE__)
end

module Memprof
  # Middleware for tracing requests
  #
  #  require 'memprof/tracer'
  #  config.middleware.use(Memprof::Tracer)
  class Tracer
    def initialize(app)
      @app=app
    end
    def call(env)
      Memprof.trace_filename ||= "/tmp/memprof_tracer-#{Process.pid}.json"
      Memprof.trace_request(env){ @app.call(env) }
    end
  end

  # Legacy filter for tracing requests on Rails 2.2
  #
  #  require 'memprof/tracer'
  #  around_filter(Memprof::Filter)
  module Filter
    def self.filter(controller)
      env = controller.request.env
      info = controller.request.path_parameters
      Memprof.trace_filename ||= "/tmp/memprof_tracer-#{Process.pid}.json"
      Memprof.trace_request(env.merge('action_controller.request.path_parameters' => info)){ yield }
    end
  end
end
