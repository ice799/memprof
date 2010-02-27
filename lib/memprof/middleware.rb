require File.expand_path('../../ext/memprof')
module Memprof
  class Middleware
    def initialize(app)
      @app = app
    end
    def call(env)
      ret = nil
      Memprof.track{
        ret = @app.call(env)
        puts
        puts '-' * 80
        puts '-' * 80
      }
      ret
    end
  end
end