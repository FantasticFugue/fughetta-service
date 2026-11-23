package fughetta.scala

import java.util.function.Consumer
import scala.reflect.internal.Phase
import scala.tools.nsc.{Global, Settings, SubComponent}
import scala.tools.nsc.reporters.StoreReporter

private[scala] final class ProjectionCompiler(settings: Settings, reporter: StoreReporter, capture: Consumer[Global#Run])
    extends Global(settings, reporter) {
  override protected def computeInternalPhases(): Unit = {
    super.computeInternalPhases()
    addToPhasesSet(new SubComponent {
      val global: ProjectionCompiler.this.type = ProjectionCompiler.this
      val phaseName = "fughetta-projection"
      val runsAfter = List("typer")
      override val runsBefore = List("superaccessors")
      val runsRightAfter = None

      def newPhase(previous: Phase): Phase = new Phase(previous) {
        def name: String = phaseName
        def run(): Unit = capture.accept(currentRun)
      }
    }, "capture typed source facts")
  }
}
